/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_owner_race.c - the race loops of Track #2373 for a test boot (not run
 * on a build host: they need nvidia.ko and nvidia-uvm.ko loaded).  Each loop
 * runs for -t seconds and passes when it makes progress the whole time (a
 * watchdog fails a stall), prints how many iterations ran and the statuses it
 * saw, and leaves nothing behind; tests/ioctl/run_owner_boot.sh then checks the
 * console, wait channels (nvcobusy) and open files.
 *
 *   1 same-process free race: T1 loops UVM_REQ_REGISTER_CHANNEL with its own fd and
 *     handle while T2 loops NV_ESC_RM_FREE plus re-allocation of that client
 *   2 close race: T1 loops registrations on the nvidiactl file T2 has just opened
 *     and allocated a client on; T2 closes it right away (the last descriptor goes
 *     while a request may hold the file)
 *   3 two users, reuse race: A loops client allocation and free; B loops
 *     allocation, free and registrations with its own handle and with A's
 *     last-seen handle (which must always be refused)
 *   4 the lock-order reproducer: one thread loops NV_ESC_RM_MAP_MEMORY naming a
 *     fresh nvidiactl fd that a second thread closes meanwhile, while another
 *     process loops root client allocation and free (the unfixed order hangs the
 *     RM here); a watchdog fails any run that does not finish
 *   5 a stranger's free of the handles next to the victim's: process S loops
 *     NV_ESC_RM_FREE of the handles just after the victim's newest client, on its
 *     own file (iterations_b counts the frees that met a client, recorded or not), while victim V loops client allocation and UVM_REQ_REGISTER_CHANNEL
 *     with its own fd and handle.  V's request may be refused while one such free
 *     is in flight, so each client is retried; a client that is refused every
 *     time (its record was lost to the stranger) fails the loop
 *
 * A loop that proves nothing is not ok: it prints RE-RUN and the program exits 4 (a loop that failed
 * exits 1, and 3 is the watchdog).  A loop is vacuous when either iteration count is 0 or, in loop 4,
 * when its open failures are at least its successful opens: a run in which the opens failed is a
 * re-run, not a pass.
 *
 *   cc -I sys -o uvm_owner_race tests/ioctl/uvm_owner_race.c -lpthread
 *   ./uvm_owner_race [-j] [-t seconds] [1|2|3|4|5 ...]     (root, for loops 3, 4 and 5)
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nvidia_uvm_ioctl.h"

#define	NV_IOCTL_MAGIC		'F'
#define	NV_ESC_RM_FREE		0x29
#define	NV_ESC_RM_ALLOC		0x2B
#define	NV_ESC_RM_MAP_MEMORY	0x4E
#define	NV01_ROOT_CLIENT	0x41

typedef struct { NvU32 hRoot, hObjectParent, hObjectOld, status; } race_nvos00;
typedef struct {
	NvU32 hRoot, hObjectParent, hObjectNew, hClass;
	NvU64 pAllocParms __attribute__((aligned(8)));
	NvU32 paramsSize, status;
} race_nvos21;
typedef struct {
	NvU32 hClient, hDevice, hMemory;
	NvU64 offset __attribute__((aligned(8)));
	NvU64 length __attribute__((aligned(8)));
	NvU64 pLinearAddress __attribute__((aligned(8)));
	NvU32 status, flags;
} race_nvos33;
typedef struct { race_nvos33 params; int fd; } race_nvos33_fd;

#define	NLOOPS	5	/* the loops below; the watchdog, the range check and the usage text count them from here */
static int argc_saved;
static char **argv_saved;
static int json, seconds = 10;

/*
 * a refusal of a free is EPERM from the ownership table (a file frees only the clients it owns) or the RM's
 * own refusal status; a handle that is no client answers with another status, never EPERM
 * (diagnostics never go into the JSON lines)
 */
static FILE *
diag(void)
{
	return (json ? stderr : stdout);
}
static atomic_int stop, progress, bad, last_bad;
static atomic_uint cur_handle;
static atomic_long iters_a, iters_b, refused, other, ioctl_failed, open_failed;

static int uvm_ioctl(int fd, uint32_t request, void *p) { return (ioctl(fd, UVM_IOCTL_NATIVE(request), p)); }

static NvU32
rm_alloc_client(int ctl)
{
	race_nvos21 a;

	memset(&a, 0, sizeof(a));
	a.hClass = NV01_ROOT_CLIENT;
	if (ioctl(ctl, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC, race_nvos21), &a) != 0 || a.status != 0)
		return (0);
	return (a.hObjectNew);
}

#define	FREE_NONE	0	/* no client there (or the free failed some other way) */
#define	FREE_REFUSED	1	/* the free reached a client and was refused (EPERM from the table, or the RM refusal): a contact */
#define	FREE_DONE	2	/* the free freed a client */

static int
rm_free_client(int ctl, NvU32 h)
{
	race_nvos00 f = { h, h, h, 0 };

	if (ioctl(ctl, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_FREE, race_nvos00), &f) != 0)
		return (errno == EPERM ? FREE_REFUSED : FREE_NONE);
	if (f.status == NV_ERR_INSUFFICIENT_PERMISSIONS)
		return (FREE_REFUSED);
	return (f.status == 0 ? FREE_DONE : FREE_NONE);
}

static int
register_channel(int uvm, int ctl, NvU32 client)
{
	UVM_REGISTER_CHANNEL_PARAMS p;

	memset(&p, 0, sizeof(p));
	p.rmCtrlFd = ctl;
	p.hClient = client;
	p.hChannel = client + 1;
	p.base = 0x10000;
	p.length = 0x10000;
	if (uvm_ioctl(uvm, UVM_REQ_REGISTER_CHANNEL, &p) != 0) {
		atomic_fetch_add(&ioctl_failed, 1);
		return (-1);
	}
	if (p.rmStatus == NV_ERR_INSUFFICIENT_PERMISSIONS)
		atomic_fetch_add(&refused, 1);
	else
		atomic_fetch_add(&other, 1);
	return ((int)p.rmStatus);
}

struct ctx { int ctl, uvm; };
static int open_pair(struct ctx *c, int init)
{
	UVM_INITIALIZE_PARAMS ip;

	c->ctl = open("/dev/nvidiactl", O_RDWR);
	c->uvm = open("/dev/nvidia-uvm", O_RDWR);
	if (c->ctl < 0 || c->uvm < 0)
		return (-1);
	memset(&ip, 0, sizeof(ip));
	if (init && (uvm_ioctl(c->uvm, UVM_REQ_INITIALIZE, &ip) != 0 || ip.rmStatus != NV_OK))
		return (-1);
	return (0);
}

static void *
t_register(void *arg)
{
	struct ctx *c = arg;

	while (!atomic_load(&stop)) {
		register_channel(c->uvm, c->ctl, atomic_load(&cur_handle));
		atomic_fetch_add(&iters_a, 1);
		atomic_fetch_add(&progress, 1);
	}
	return (NULL);
}

static void *
t_realloc(void *arg)
{
	struct ctx *c = arg;
	NvU32 h;

	while (!atomic_load(&stop)) {
		h = atomic_load(&cur_handle);
		rm_free_client(c->ctl, h);
		h = rm_alloc_client(c->ctl);
		if (h != 0)
			atomic_store(&cur_handle, h);
		atomic_fetch_add(&iters_b, 1);
		atomic_fetch_add(&progress, 1);
	}
	return (NULL);
}

static atomic_int pub_fd = -1;
static atomic_uint pub_handle;

/* loop 2, thread 2: a fresh nvidiactl file and client each time, published, then closed while a request may hold it */
static void *
t_open_close(void *arg)
{
	struct ctx *c = arg;
	NvU32 h;
	int fd;

	(void)c;
	while (!atomic_load(&stop)) {
		fd = open("/dev/nvidiactl", O_RDWR);
		if (fd < 0)
			continue;
		h = rm_alloc_client(fd);
		atomic_store(&pub_handle, h);
		atomic_store(&pub_fd, fd);
		sched_yield();
		close(fd);
		atomic_fetch_add(&iters_b, 1);
		atomic_fetch_add(&progress, 1);
	}
	return (NULL);
}

/* loop 2, thread 1: registrations on whatever file is published (it may be closing: refused or a normal RM status) */
static void *
t_register_pub(void *arg)
{
	struct ctx *c = arg;

	while (!atomic_load(&stop)) {
		register_channel(c->uvm, atomic_load(&pub_fd), atomic_load(&pub_handle));
		atomic_fetch_add(&iters_a, 1);
		atomic_fetch_add(&progress, 1);
	}
	return (NULL);
}

/* loop 4, thread 1: MAP_MEMORY naming a fresh nvidiactl file that thread 2 closes at once */
static void *
t_map_close(void *arg)
{
	struct ctx *c = arg;
	race_nvos33_fd m;
	int fd, old;

	while (!atomic_load(&stop)) {
		fd = open("/dev/nvidiactl", O_RDWR);
		if (fd < 0) {
			atomic_fetch_add(&open_failed, 1);	/* counted, not silent; the closer frees descriptors */
			sched_yield();
			continue;
		}
		old = atomic_exchange(&pub_fd, fd);
		if (old >= 0 && old != fd)	/* the closer takes its descriptor by exchange too, so each is closed once */
			close(old);	/* the closer did not get to it: never leak a descriptor */
		memset(&m, 0, sizeof(m));
		m.params.hClient = atomic_load(&cur_handle);
		m.fd = fd;
		(void)ioctl(c->ctl, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_MAP_MEMORY, race_nvos33_fd), &m);
		atomic_fetch_add(&iters_a, 1);
		atomic_fetch_add(&progress, 1);
	}
	return (NULL);
}

static void *
t_closer(void *arg)
{
	int fd;

	(void)arg;
	while (!atomic_load(&stop)) {
		fd = atomic_exchange(&pub_fd, -1);
		if (fd >= 0)
			close(fd);	/* the last descriptor of that file: the close runs inside or after the RM call */
		atomic_fetch_add(&iters_b, 1);
	}
	return (NULL);
}

static void
run_threads(void *(*f1)(void *), void *(*f2)(void *), struct ctx *c)
{
	pthread_t t1, t2;
	int last = -1, stalled = 0, s;

	atomic_store(&stop, 0);
	atomic_store(&progress, 0);
	pthread_create(&t1, NULL, f1, c);
	pthread_create(&t2, NULL, f2, c);
	for (s = 0; s < seconds; s++) {
		sleep(1);
		if (atomic_load(&progress) == last) {
			if (++stalled >= 5) {
				atomic_store(&bad, 1);
				fprintf(diag(), "FAIL: no progress for 5 s: a deadlock\n");
				fflush(diag());
				_exit(3);	/* the threads are stuck in the kernel */
			}
		} else {
			stalled = 0;
			last = atomic_load(&progress);
		}
	}
	atomic_store(&stop, 1);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);
}

/* the one rule: vacuous when either count is 0 or, in loop 4, when the open failures are at least the successful opens (no division) */
static int
vacuous_loop(int loop, long a, long b, long open_fail)
{
	return (a == 0 || b == 0 || (loop == 4 && 2 * open_fail >= a + open_fail));
}

static void
report(int loop, const char *what)
{
	long a = atomic_load(&iters_a), b = atomic_load(&iters_b);
	int vac = vacuous_loop(loop, a, b, (long)atomic_load(&open_failed));
	int failed = atomic_load(&bad);	/* a check that failed before the vacuity rule: never hidden by it */

	if (vac) {
		atomic_store(&bad, 1);
		fprintf(diag(), "RE-RUN: loop %d is vacuous (iterations %ld + %ld, %ld open failures): nothing was proved\n",
		    loop, a, b, (long)atomic_load(&open_failed));
	}
	if (json)
		printf("{\"schema\":\"uvm-owner-race/1\",\"loop\":%d,\"what\":\"%s\",\"seconds\":%d,\"iterations_a\":%ld,"
		    "\"iterations_b\":%ld,\"refused\":%ld,\"other_status\":%ld,\"ioctl_failed\":%ld,\"open_failed\":%ld,\"failed\":%s,\"vacuous\":%s,\"ok\":%s}\n",
		    loop, what, seconds, a, b, (long)atomic_load(&refused), (long)atomic_load(&other),
		    (long)atomic_load(&ioctl_failed), (long)atomic_load(&open_failed), failed ? "true" : "false",
		    vac ? "true" : "false", atomic_load(&bad) ? "false" : "true");
	else
		printf("%s loop %d (%s): %ld + %ld iterations, %ld refused, %ld other statuses, %ld ioctl failures, %ld open failures\n",
		    failed ? "FAIL" : (vac ? "RE-RUN" : "ok  "), loop, what, a, b, (long)atomic_load(&refused),
		    (long)atomic_load(&other), (long)atomic_load(&ioctl_failed), (long)atomic_load(&open_failed));
	fflush(stdout);
	atomic_store(&iters_a, 0); atomic_store(&iters_b, 0); atomic_store(&refused, 0);
	atomic_store(&other, 0); atomic_store(&ioctl_failed, 0); atomic_store(&open_failed, 0);
	/* this loop's verdict (1 failed, 4 vacuous only); the next loop starts clean */
	atomic_store(&last_bad, failed ? 1 : (vac ? 4 : 0));
	atomic_store(&bad, 0);
}

static int
loop1(void)
{
	struct ctx c;

	if (open_pair(&c, 1) != 0)
		return (2);
	atomic_store(&cur_handle, rm_alloc_client(c.ctl));
	run_threads(t_register, t_realloc, &c);
	report(1, "same-process free and re-allocation against registrations");
	close(c.ctl); close(c.uvm);
	return (atomic_load(&last_bad));
}

static int
loop2(void)
{
	struct ctx c;

	if (open_pair(&c, 1) != 0)
		return (2);
	atomic_store(&pub_fd, -1);
	run_threads(t_register_pub, t_open_close, &c);
	report(2, "registrations against a nvidiactl file closed while in use");
	close(c.ctl); close(c.uvm);
	return (atomic_load(&last_bad));
}

static int
become(unsigned uid)
{
	return (setgroups(0, NULL) != 0 || setgid(uid) != 0 || setuid(uid) != 0 ? -1 : 0);
}

struct shared {
	atomic_uint a_handle;	/* user A's latest client handle */
	atomic_long violations;	/* a foreign handle that was not refused */
	atomic_long foreign;	/* foreign-handle registrations made */
	atomic_long allocs;	/* B's client allocations */
	atomic_uint v_handle;	/* loop 5: the victim's newest client handle */
	atomic_long stuck;	/* loop 5: clients refused on every retry */
	atomic_long first_refused;	/* loop 5: clients refused on the first request (the free in flight) */
	atomic_long v_clients;	/* loop 5: clients the victim allocated and used */
	atomic_long s_frees;	/* loop 5: the stranger's rounds */
	atomic_long s_freed;	/* loop 5: clients the stranger freed (a violation) */
	atomic_long v_free_refused;	/* loop 5: the victim's own free of its client refused (a violation) */
	atomic_long s_hits;	/* loop 5: the stranger's frees that met a client (and were refused) */
	atomic_int v_done;	/* loop 5: the victim finished */
};

static void
user_a(struct shared *sh)
{
	struct ctx c;
	NvU32 h;
	time_t end = time(NULL) + seconds;

	if (become(65534) != 0 || open_pair(&c, 0) != 0)
		_exit(2);
	while (time(NULL) < end) {
		h = rm_alloc_client(c.ctl);
		if (h != 0) {
			atomic_store(&sh->a_handle, h);
			rm_free_client(c.ctl, h);
		}
	}
	_exit(0);
}

static void
user_b(struct shared *sh)
{
	struct ctx c;
	NvU32 h, a;
	int st;
	time_t end = time(NULL) + seconds;

	if (become(65533) != 0 || open_pair(&c, 1) != 0)
		_exit(2);
	while (time(NULL) < end) {
		h = rm_alloc_client(c.ctl);
		if (h != 0) {
			atomic_fetch_add(&sh->allocs, 1);
			register_channel(c.uvm, c.ctl, h);		/* its own handle */
			a = atomic_load(&sh->a_handle);
			if (a != 0 && a != h) {			/* A's handle, on B's own file: always refused */
				st = register_channel(c.uvm, c.ctl, a);
				atomic_fetch_add(&sh->foreign, 1);
				if (st != (int)NV_ERR_INSUFFICIENT_PERMISSIONS)
					atomic_fetch_add(&sh->violations, 1);
			}
			rm_free_client(c.ctl, h);
		}
	}
	_exit(atomic_load(&sh->violations) != 0 ? 1 : 0);
}

/* shared counters and two child processes, each running f(shared); NULL when either could not start */
static struct shared *
spawn_two(void (*f1)(struct shared *), void (*f2)(struct shared *), pid_t *p1, pid_t *p2)
{
	struct shared *sh;

	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
	if (sh == MAP_FAILED)
		return (NULL);
	memset(sh, 0, sizeof(*sh));
	*p1 = fork();
	if (*p1 == 0)
		f1(sh);
	if (*p1 < 0) {
		munmap(sh, sizeof(*sh));
		return (NULL);
	}
	*p2 = fork();
	if (*p2 == 0)
		f2(sh);
	if (*p2 < 0) {		/* never leave the first child running */
		kill(*p1, SIGKILL);
		waitpid(*p1, NULL, 0);
		munmap(sh, sizeof(*sh));
		return (NULL);
	}
	return (sh);
}

static int
loop3(void)
{
	struct shared *sh;
	pid_t pa, pb;
	int sa, sb;

	sh = spawn_two(user_a, user_b, &pa, &pb);
	if (sh == NULL)
		return (2);
	waitpid(pa, &sa, 0);
	waitpid(pb, &sb, 0);
	atomic_store(&iters_a, atomic_load(&sh->foreign));
	atomic_store(&iters_b, atomic_load(&sh->allocs));
	if (atomic_load(&sh->violations) != 0) {
		atomic_store(&bad, 1);
		fprintf(diag(), "FAIL: %ld foreign-handle requests were not refused\n", (long)atomic_load(&sh->violations));
	}
	if (!WIFEXITED(sa) || WEXITSTATUS(sa) != 0 || !WIFEXITED(sb) || WEXITSTATUS(sb) != 0) {
		atomic_store(&bad, 1);
		fprintf(diag(), "FAIL: loop 3: a user process did not exit 0 (2: could not change uid or open the device)\n");
	}
	report(3, "two users allocating, freeing and registering (iterations_a = foreign-handle requests, iterations_b = allocations)");
	return (atomic_load(&last_bad));
}

static int
loop4(void)
{
	pid_t pid;
	struct ctx c;
	NvU32 h;
	int s;

	pid = fork();
	if (pid < 0)
		return (2);
	if (pid == 0) {		/* another process: root client allocation and free */
		time_t end = time(NULL) + seconds;

		if (open_pair(&c, 0) != 0)
			_exit(2);
		while (time(NULL) < end) {
			h = rm_alloc_client(c.ctl);
			if (h != 0)
				rm_free_client(c.ctl, h);
		}
		_exit(0);
	}
	if (open_pair(&c, 0) != 0)
		return (2);
	atomic_store(&cur_handle, rm_alloc_client(c.ctl));
	atomic_store(&pub_fd, -1);
	run_threads(t_map_close, t_closer, &c);
	if (atomic_load(&pub_fd) >= 0)
		close(atomic_exchange(&pub_fd, -1));	/* the descriptor still published when the loop stopped */
	waitpid(pid, &s, 0);
	if (!WIFEXITED(s) || WEXITSTATUS(s) != 0) {
		atomic_store(&bad, 1);
		fprintf(diag(), "FAIL: loop 4: the allocator process did not exit 0 (2: could not open the device)\n");
	}
	report(4, "MAP_MEMORY naming a file that is closed meanwhile, against client allocation and free");
	return (atomic_load(&last_bad));
}

#define	STRANGER_FREES	3	/* the handles after the victim's newest one that the stranger frees each time: it assumes
				 * the RM hands client handles out in sequence; s_hits counts the frees that met a client, and
				 * a loop with no hit is vacuous */
#define	UID_STRANGER	60001	/* dedicated uids: the loop frees handles that may be someone else's if the rule is wrong */
#define	UID_VICTIM	60002
#define	RETRIES		200	/* a refusal that outlasts this many retries (a free is over in microseconds) is a lost record */

static void
stranger(struct shared *sh)
{
	struct ctx c;
	NvU32 h;
	int k, r;
	time_t end = time(NULL) + seconds + 5;	/* a backstop: the victim ends the loop, a dead parent must not leave this running */

	if (become(UID_STRANGER) != 0)
		_exit(2);
	c.ctl = open("/dev/nvidiactl", O_RDWR);
	if (c.ctl < 0)
		_exit(2);
	while (!atomic_load(&sh->v_done) && time(NULL) < end) {
		h = atomic_load(&sh->v_handle);
		if (h == 0) {
			sched_yield();
			continue;
		}
		for (k = 1; k <= STRANGER_FREES; k++) {
			r = rm_free_client(c.ctl, h + k);	/* the next handles: not recorded yet, or not clients */
			if (r == FREE_REFUSED)
				atomic_fetch_add(&sh->s_hits, 1);
			else if (r == FREE_DONE)
				atomic_fetch_add(&sh->s_freed, 1);	/* the stranger owns no client: any it freed is a violation */
		}
		atomic_fetch_add(&sh->s_frees, 1);
	}
	_exit(0);
}

static void
victim(struct shared *sh)
{
	struct ctx c;
	NvU32 h;
	int i, st = 0;
	time_t end = time(NULL) + seconds;

	if (become(UID_VICTIM) != 0 || open_pair(&c, 1) != 0)
		_exit(2);
	while (time(NULL) < end) {
		h = rm_alloc_client(c.ctl);
		if (h == 0)
			continue;
		atomic_store(&sh->v_handle, h);
		for (i = 0; i < RETRIES; i++) {
			st = register_channel(c.uvm, c.ctl, h);
			if (st != (int)NV_ERR_INSUFFICIENT_PERMISSIONS)
				break;
			if (i == 0)
				atomic_fetch_add(&sh->first_refused, 1);
			usleep(100);
		}
		if (st == (int)NV_ERR_INSUFFICIENT_PERMISSIONS)
			atomic_fetch_add(&sh->stuck, 1);	/* the owner never got its client back */
		atomic_fetch_add(&sh->v_clients, 1);
		if (rm_free_client(c.ctl, h) == FREE_REFUSED)
			atomic_fetch_add(&sh->v_free_refused, 1);	/* the owner could not free its own client */
	}
	atomic_store(&sh->v_done, 1);
	_exit(atomic_load(&sh->stuck) != 0 || atomic_load(&sh->v_free_refused) != 0 ? 1 : 0);
}

static int
loop5(void)
{
	struct shared *sh;
	pid_t ps, pv;
	int ss, sv;

	sh = spawn_two(stranger, victim, &ps, &pv);
	if (sh == NULL)
		return (2);
	waitpid(pv, &sv, 0);
	atomic_store(&sh->v_done, 1);
	waitpid(ps, &ss, 0);
	atomic_store(&iters_a, atomic_load(&sh->v_clients));
	atomic_store(&iters_b, atomic_load(&sh->s_hits));
	atomic_store(&refused, atomic_load(&sh->first_refused));
	if (atomic_load(&sh->s_freed) != 0 || atomic_load(&sh->v_free_refused) != 0) {
		atomic_store(&bad, 1);
		fprintf(diag(), "FAIL: the stranger freed %ld clients it does not own, the victim was refused %ld frees of its own\n",
		    (long)atomic_load(&sh->s_freed), (long)atomic_load(&sh->v_free_refused));
	}
	if (atomic_load(&sh->stuck) != 0) {
		atomic_store(&bad, 1);
		fprintf(diag(), "FAIL: %ld clients were refused on every one of %d retries: a stranger's free cost the owner its record\n",
		    (long)atomic_load(&sh->stuck), RETRIES);
	}
	if (!WIFEXITED(sv) || WEXITSTATUS(sv) != 0 || !WIFEXITED(ss) || WEXITSTATUS(ss) != 0) {
		atomic_store(&bad, 1);
		fprintf(diag(), "FAIL: loop 5: the victim or the stranger did not exit 0 (2: could not change uid or open the device)\n");
	}
	report(5, "a stranger frees the handles after the victim's newest client (iterations_a = victim clients, iterations_b = stranger frees that met a client, refused = first requests refused)");
	return (atomic_load(&last_bad));
}

static void
watchdog(int sig)
{
	(void)sig;
	fprintf(diag(), "FAIL: the run did not finish in time: a deadlock\n");
	fflush(diag());
	_exit(3);
}

static int
parse_args(int argc, char **argv)
{
	int c, i;

	while ((c = getopt(argc, argv, "jt:")) != -1) {
		if (c == 'j')
			json = 1;
		else if (c == 't')
			seconds = atoi(optarg);
		else
			goto usage;
	}
	if (seconds < 1) {
		printf("-t must be at least 1\n");
		return (-1);
	}
	for (i = optind; i < argc; i++) {
		if (atoi(argv[i]) < 1 || atoi(argv[i]) > NLOOPS) {
			printf("unknown loop '%s' (1 to %d)\n", argv[i], NLOOPS);
			return (-1);
		}
	}
	return (0);
usage:
	printf("usage: uvm_owner_race [-j] [-t seconds] [1|2|3|4|5 ...]\n");
	return (-1);
}

static int
run_loop(int which)
{
	switch (which) {
	case 1: return (loop1());
	case 2: return (loop2());
	case 3: return (loop3());
	case 4: return (loop4());
	case 5: return (loop5());
	default: return (2);	/* a loop number that parse_args let through but nobody runs: never a pass */
	}
}

static int
wanted_loop(int c)
{
	int i;

	if (optind >= argc_saved)	/* no loop number named: all of them */
		return (1);
	for (i = optind; i < argc_saved; i++)
		if (atoi(argv_saved[i]) == c)
			return (1);
	return (0);
}

static int
wanted_loops(void)
{
	int c, n = 0;

	for (c = 1; c <= NLOOPS; c++)
		n += wanted_loop(c);
	return (n);
}

int
main(int argc, char **argv)
{
	int c, r, rc = 0, ran = 0;

	argc_saved = argc;
	argv_saved = argv;
	if (parse_args(argc, argv) != 0)
		return (2);
	signal(SIGALRM, watchdog);
	alarm((unsigned)(seconds * wanted_loops() + 40));	/* every loop, including the process loops, is under a watchdog */
	for (c = 1; c <= NLOOPS; c++) {
		if (!wanted_loop(c))
			continue;
		r = run_loop(c);
		ran++;
		if (r != 0 && (r == 1 || rc == 0 || rc == 4))	/* a failed loop (1) outranks everything; vacuous (4) ranks last */
			rc = r;
	}
	return (ran == 0 ? 2 : rc);
}
