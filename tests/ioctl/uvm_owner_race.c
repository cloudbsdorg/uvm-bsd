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
 *   1 same-process free race: T1 loops UVM_REGISTER_CHANNEL with its own fd and
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
 *
 *   cc -I sys -o uvm_owner_race tests/ioctl/uvm_owner_race.c -lpthread
 *   ./uvm_owner_race [-j] [-t seconds] [1|2|3|4 ...]       (root, for loop 3 and 4)
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

static int json, seconds = 10;
static atomic_int stop, progress, bad;
static atomic_uint cur_handle;
static atomic_long iters_a, iters_b, refused, other, ioctl_failed, open_failed;

static int uvm_ioctl(int fd, unsigned long raw, void *p) { return (ioctl(fd, UVM_IOCTL_NATIVE(raw), p)); }

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

static void
rm_free_client(int ctl, NvU32 h)
{
	race_nvos00 f = { h, h, h, 0 };

	(void)ioctl(ctl, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_FREE, race_nvos00), &f);
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
	if (uvm_ioctl(uvm, UVM_REGISTER_CHANNEL, &p) != 0) {
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
	if (init && (uvm_ioctl(c->uvm, UVM_INITIALIZE, &ip) != 0 || ip.rmStatus != NV_OK))
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
				printf("FAIL: no progress for 5 s: a deadlock\n");
				fflush(stdout);
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

static void
report(int loop, const char *what)
{
	long a = atomic_load(&iters_a), b = atomic_load(&iters_b);

	if (json)
		printf("{\"schema\":\"uvm-owner-race/1\",\"loop\":%d,\"what\":\"%s\",\"seconds\":%d,\"iterations_a\":%ld,"
		    "\"iterations_b\":%ld,\"refused\":%ld,\"other_status\":%ld,\"ioctl_failed\":%ld,\"open_failed\":%ld,\"ok\":%s}\n",
		    loop, what, seconds, a, b, (long)atomic_load(&refused), (long)atomic_load(&other),
		    (long)atomic_load(&ioctl_failed), (long)atomic_load(&open_failed), atomic_load(&bad) ? "false" : "true");
	else
		printf("%s loop %d (%s): %ld + %ld iterations, %ld refused, %ld other statuses, %ld ioctl failures, %ld open failures\n",
		    atomic_load(&bad) ? "FAIL" : "ok  ", loop, what, a, b, (long)atomic_load(&refused),
		    (long)atomic_load(&other), (long)atomic_load(&ioctl_failed), (long)atomic_load(&open_failed));
	fflush(stdout);
	atomic_store(&iters_a, 0); atomic_store(&iters_b, 0); atomic_store(&refused, 0);
	atomic_store(&other, 0); atomic_store(&ioctl_failed, 0); atomic_store(&open_failed, 0);
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
	return (0);
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
	return (atomic_load(&bad) ? 1 : 0);
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

static int
loop3(void)
{
	struct shared *sh;
	pid_t pa, pb;
	int sa, sb;

	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
	if (sh == MAP_FAILED)
		return (2);
	memset(sh, 0, sizeof(*sh));
	pa = fork();
	if (pa == 0)
		user_a(sh);
	pb = fork();
	if (pb == 0)
		user_b(sh);
	if (pa < 0 || pb < 0)
		return (2);
	waitpid(pa, &sa, 0);
	waitpid(pb, &sb, 0);
	atomic_store(&iters_a, atomic_load(&sh->foreign));
	if (atomic_load(&sh->violations) != 0) {
		atomic_store(&bad, 1);
		printf("FAIL: %ld foreign-handle requests were not refused\n", (long)atomic_load(&sh->violations));
	}
	if (!WIFEXITED(sa) || WEXITSTATUS(sa) != 0 || !WIFEXITED(sb) || WEXITSTATUS(sb) != 0)
		atomic_store(&bad, 1);
	report(3, "two users allocating, freeing and registering (iterations_a = foreign-handle requests)");
	return (atomic_load(&bad) ? 1 : 0);
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
	if (!WIFEXITED(s) || WEXITSTATUS(s) != 0)
		atomic_store(&bad, 1);
	report(4, "MAP_MEMORY naming a file that is closed meanwhile, against client allocation and free");
	return (atomic_load(&bad) ? 1 : 0);
}

static void
watchdog(int sig)
{
	(void)sig;
	printf("FAIL: the run did not finish in time: a deadlock\n");
	fflush(stdout);
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
		if (atoi(argv[i]) < 1 || atoi(argv[i]) > 4) {
			printf("unknown loop '%s' (1 to 4)\n", argv[i]);
			return (-1);
		}
	}
	return (0);
usage:
	printf("usage: uvm_owner_race [-j] [-t seconds] [1|2|3|4 ...]\n");
	return (-1);
}

static int
run_loop(int which)
{
	switch (which) {
	case 1: return (loop1());
	case 2: return (loop2());
	case 3: return (loop3());
	default: return (loop4());
	}
}

int
main(int argc, char **argv)
{
	int c, i, r, rc = 0, ran = 0;

	if (parse_args(argc, argv) != 0)
		return (2);
	signal(SIGALRM, watchdog);
	alarm((unsigned)(seconds * 4 + 40));	/* every loop, including the process loops, is under a watchdog */
	for (c = 1; c <= 4; c++) {
		/* no loop number named: all four; otherwise only those named */
		int wanted = optind >= argc;

		for (i = optind; i < argc; i++)
			wanted |= atoi(argv[i]) == c;
		if (!wanted)
			continue;
		r = run_loop(c);
		ran++;
		if (r != 0)
			rc = r;
	}
	return (ran == 0 ? 2 : rc | (atomic_load(&bad) ? 1 : 0));
}
