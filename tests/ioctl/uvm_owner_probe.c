/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_owner_probe.c - the two-user adversarial probe of Track #2373: user B
 * names user A's RM client handle in the four requests that carry an
 * rmCtrlFd and reach RM, and every attempt must be refused with
 * NV_ERR_INSUFFICIENT_PERMISSIONS before RM sees the handle.
 *
 *   cc -I sys -o uvm_owner_probe tests/ioctl/uvm_owner_probe.c
 *   (as root, with nvidia.ko and nvidia-uvm.ko loaded; needs no GPU registration)
 *   ./uvm_owner_probe [-v] [-j] [-a uidA] [-b uidB]     (defaults 65534 and 65533; -j: one JSON object per row)
 *
 * Each user, in its own process (supplementary groups dropped), opens /dev/nvidiactl
 * and /dev/nvidia-uvm, allocates an RM client (NV01_ROOT_CLIENT, the RM assigns the
 * handle), initialises UVM and passes its nvidiactl file to the other over a unix
 * socket.  Then, for register GPU, register GPU VA space, register channel, map
 * external allocation and alloc device P2P (and register GPU with an SMC reference but
 * no client), with the OTHER's handle:
 *   - own nvidiactl fd, /dev/null, /dev/zero, a closed fd, an unopened number,
 *     /dev/nvidia0 (when present), a dup of the own fd          -> refused
 *   - the other's nvidiactl file passed to us (the capability)  -> NOT refused
 * and with the own handle: the own fd and a dup of it -> NOT refused for ownership
 * (the request fails later for want of a registered GPU, which is the point: the check
 * let it through); an SMC reference with no client is refused even for the owner.
 * An ioctl that fails itself is a FAIL, never a pass.  -j prints one JSON object per
 * row (schema uvm-owner-probe/1) and a summary object per user.
 * The handles are printed: they are predictable (base 0xC1D00000), which is
 * why a handle is a name and not a secret.  Exit 0 only when every row held.
 */

#include <sys/types.h>
#include <sys/capsicum.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvidia_uvm_ioctl.h"

#define	NV_IOCTL_MAGIC		'F'
#define	NV_ESC_RM_ALLOC		0x2B
#define	NV01_ROOT		0x0

/* NVOS21_PARAMETERS of nvos.h (open-gpu-kernel-modules, MIT) */
typedef struct {
	NvU32	hRoot;
	NvU32	hObjectParent;
	NvU32	hObjectNew;
	NvU32	hClass;
	NvU64	pAllocParms __attribute__((aligned(8)));
	NvU32	paramsSize;
	NvU32	status;
} probe_nvos21;

static int verbose, fails, rows;

struct side {
	int	ctl;		/* this side's nvidiactl fd */
	int	uvm;		/* this side's nvidia-uvm fd */
	NvU32	client;		/* the RM client allocated on ctl */
};

static int
uvm_ioctl(int fd, unsigned long raw, void *params)
{
	errno = 0;
	return (ioctl(fd, UVM_IOCTL_NATIVE(raw), params));
}

static int
setup(struct side *s)
{
	probe_nvos21 a;
	UVM_INITIALIZE_PARAMS ip;

	s->ctl = open("/dev/nvidiactl", O_RDWR);
	if (s->ctl < 0) {
		printf("cannot open /dev/nvidiactl: %s (is nvidia.ko loaded?)\n", strerror(errno));
		return (-1);
	}
	s->uvm = open("/dev/nvidia-uvm", O_RDWR);
	if (s->uvm < 0) {
		printf("cannot open /dev/nvidia-uvm: %s (is nvidia-uvm.ko loaded?)\n", strerror(errno));
		return (-1);
	}
	memset(&a, 0, sizeof(a));
	a.hClass = NV01_ROOT;
	if (ioctl(s->ctl, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC, probe_nvos21), &a) != 0 || a.status != 0) {
		printf("RM client allocation failed: errno %d status 0x%x\n", errno, a.status);
		return (-1);
	}
	s->client = a.hObjectNew;
	memset(&ip, 0, sizeof(ip));
	if (uvm_ioctl(s->uvm, UVM_INITIALIZE, &ip) != 0 || ip.rmStatus != NV_OK) {
		printf("UVM_INITIALIZE failed: errno %d status 0x%x\n", errno, ip.rmStatus);
		return (-1);
	}
	return (0);
}

#define	IOCTL_FAILED	(~0u)

static int last_errno;

/* the four requests; returns rmStatus, or IOCTL_FAILED when the ioctl itself failed (errno in last_errno) */
static NvU32
request(int uvm, int which, int ctl_fd, NvU32 client)
{
	NvU32 st;
	int r;

	switch (which) {
	case 0: {
		UVM_REGISTER_GPU_PARAMS p;

		memset(&p, 0, sizeof(p));
		p.rmCtrlFd = ctl_fd;
		p.hClient = client;
		r = uvm_ioctl(uvm, UVM_REGISTER_GPU, &p);
		st = p.rmStatus;
		break;
	}
	case 1: {
		UVM_REGISTER_GPU_VASPACE_PARAMS p;

		memset(&p, 0, sizeof(p));
		p.rmCtrlFd = ctl_fd;
		p.hClient = client;
		p.hVaSpace = client + 1;
		r = uvm_ioctl(uvm, UVM_REGISTER_GPU_VASPACE, &p);
		st = p.rmStatus;
		break;
	}
	case 2: {
		UVM_REGISTER_CHANNEL_PARAMS p;

		memset(&p, 0, sizeof(p));
		p.rmCtrlFd = ctl_fd;
		p.hClient = client;
		p.hChannel = client + 1;
		p.base = 0x10000;
		p.length = 0x10000;
		r = uvm_ioctl(uvm, UVM_REGISTER_CHANNEL, &p);
		st = p.rmStatus;
		break;
	}
	case 4: {
		UVM_ALLOC_DEVICE_P2P_PARAMS p;

		memset(&p, 0, sizeof(p));
		p.rmCtrlFd = ctl_fd;
		p.hClient = client;
		p.hMemory = client + 1;
		p.length = 0x10000;
		r = uvm_ioctl(uvm, UVM_ALLOC_DEVICE_P2P, &p);
		st = p.rmStatus;
		break;
	}
	case 5: {		/* an SMC partition reference without its client */
		UVM_REGISTER_GPU_PARAMS p;

		memset(&p, 0, sizeof(p));
		p.rmCtrlFd = ctl_fd;
		p.hClient = 0;
		p.hSmcPartRef = 5;
		r = uvm_ioctl(uvm, UVM_REGISTER_GPU, &p);
		st = p.rmStatus;
		break;
	}
	default: {
		UVM_MAP_EXTERNAL_ALLOCATION_PARAMS p;

		memset(&p, 0, sizeof(p));
		p.base = 0x200000;
		p.length = 0x10000;
		p.rmCtrlFd = ctl_fd;
		p.hClient = client;
		p.hMemory = client + 1;
		p.gpuAttributesCount = 1;
		r = uvm_ioctl(uvm, UVM_MAP_EXTERNAL_ALLOCATION, &p);
		st = p.rmStatus;
		break;
	}
	}
	last_errno = errno;
	return (r != 0 ? IOCTL_FAILED : st);
}

#define	NREQUESTS	6
static const char *const names[] = { "register GPU", "register GPU VA space", "register channel",
    "map external allocation", "alloc device P2P", "register GPU (SMC reference, no client)" };
static int json;

static void
emit_json(const char *who, const char *what, int which, NvU32 got, int refused, int ok)
{
	printf("{\"schema\":\"uvm-owner-probe/1\",\"who\":\"%s\",\"row\":\"%s\",\"request\":\"%s\","
	    "\"expected\":\"%s\",\"status\":%u,\"ioctl_failed\":%s,\"errno\":%d,\"ok\":%s}\n",
	    who, what, names[which], refused ? "refused" : "not refused", got == IOCTL_FAILED ? 0u : (unsigned)got,
	    got == IOCTL_FAILED ? "true" : "false", got == IOCTL_FAILED ? last_errno : 0, ok ? "true" : "false");
	fflush(stdout);
}

static void
expect(const char *who, const char *what, int which, NvU32 got, int refused)
{
	int ok;

	rows++;
	if (got == IOCTL_FAILED) {
		/* never a pass: the check was not exercised */
		fails++;
		if (json) {
			emit_json(who, what, which, got, refused, 0);
			return;
		}
		printf("FAIL %s, %s, %s: the ioctl itself failed, errno %d (%s)\n", who, what, names[which],
		    last_errno, strerror(last_errno));
		return;
	}
	ok = refused ? got == NV_ERR_INSUFFICIENT_PERMISSIONS : got != NV_ERR_INSUFFICIENT_PERMISSIONS;
	if (!ok)
		fails++;
	if (json)
		emit_json(who, what, which, got, refused, ok);
	else if (!ok || verbose)
		printf("%s %s, %s, %s: status 0x%x\n", ok ? "ok  " : "FAIL", who, what, names[which],
		    (unsigned)got);
}

static void
skipped(const char *who, const char *what, int which)
{
	if (json)
		printf("{\"schema\":\"uvm-owner-probe/1\",\"who\":\"%s\",\"row\":\"%s\",\"request\":\"%s\",\"skipped\":true}\n",
		    who, what, names[which]);
	else if (verbose)
		printf("skip %s, %s, %s\n", who, what, names[which]);
}

/*
 * what `me` does with `other`'s handle and fds, and with its own.  passed_fd is the other side's nvidiactl
 * file passed to us over a unix socket (SCM_RIGHTS), or -1: possession of the open file is the capability,
 * so the other's handle through it is NOT refused.
 */
static void
attack(const char *who, struct side *me, NvU32 other_client, int passed_fd)
{
	int w, nullfd, zerofd, closedfd, devfd, dupfd, never_open = 1000;

	devfd = open("/dev/nvidia0", O_RDWR);	/* a device file: -1 when the node is absent, the row is skipped */
	for (w = 0; w < NREQUESTS; w++) {
		nullfd = open("/dev/null", O_RDWR);
		zerofd = open("/dev/zero", O_RDWR);
		dupfd = dup(me->ctl);
		closedfd = dup(me->ctl);
		close(closedfd);		/* a number that was open a moment ago and is not any more */
		if (w == 5) {	/* an SMC reference with no client: refused whoever asks, owner included */
			expect(who, "own fd, no client, an SMC reference", w, request(me->uvm, w, me->ctl, 0), 1);
			close(nullfd); close(zerofd); close(dupfd);
			continue;
		}
		expect(who, "own fd, the other's handle", w, request(me->uvm, w, me->ctl, other_client), 1);
		expect(who, "a non-nvidia fd (/dev/null)", w, request(me->uvm, w, nullfd, other_client), 1);
		expect(who, "another open file (/dev/zero)", w, request(me->uvm, w, zerofd, other_client), 1);
		expect(who, "a closed fd", w, request(me->uvm, w, closedfd, other_client), 1);
		expect(who, "a descriptor number that is not open", w, request(me->uvm, w, never_open, other_client), 1);
		if (devfd >= 0)
			expect(who, "a device file (/dev/nvidia0), the other's handle", w,
			    request(me->uvm, w, devfd, other_client), 1);
		else
			skipped(who, "a device file (/dev/nvidia0): the node is absent", w);
		if (passed_fd >= 0)
			expect(who, "the other's nvidiactl file passed to us (the capability), the other's handle", w,
			    request(me->uvm, w, passed_fd, other_client), 0);
		else {
			fails++;
			rows++;
			printf("FAIL %s, the other's nvidiactl file was not passed to us: the capability row cannot run\n", who);
		}
		{	/* a descriptor whose ioctl list is limited (cap_ioctls_limit): refused, the list does not bind this path */
			int limfd = dup(me->ctl);
			unsigned long one = 0;

			if (limfd >= 0 && cap_ioctls_limit(limfd, &one, 1) == 0)
				expect(who, "a dup of own fd with a limited ioctl list, own handle", w,
				    request(me->uvm, w, limfd, me->client), 1);
			else {
				fails++;
				rows++;
				printf("FAIL %s, cannot build the limited-ioctl descriptor: %s\n", who, strerror(errno));
			}
			if (limfd >= 0)
				close(limfd);
		}
		expect(who, "own fd, own handle (owner)", w, request(me->uvm, w, me->ctl, me->client), 0);
		expect(who, "a dup of own fd, own handle (the same file)", w, request(me->uvm, w, dupfd, me->client), 0);
		expect(who, "a dup of own fd, the other's handle", w, request(me->uvm, w, dupfd, other_client), 1);
		close(nullfd); close(zerofd); close(dupfd);
	}
	if (devfd >= 0)
		close(devfd);
}

static int
become(const char *arg, uid_t def)
{
	char *end;
	uid_t u = def;

	if (arg != NULL) {
		u = (uid_t)strtoul(arg, &end, 10);
		if (*arg == '\0' || *end != '\0' || u == 0) {
			printf("not a usable uid: '%s' (a number other than 0)\n", arg);
			return (-1);
		}
	}

	if (setgroups(0, NULL) != 0 || setgid(u) != 0 || setuid(u) != 0) {
		printf("cannot become uid %u: %s\n", (unsigned)u, strerror(errno));
		return (-1);
	}
	return (0);
}

static int
send_fd(int sock, int fd)
{
	struct msghdr m;
	struct iovec iov;
	char b = 'F', ctl[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *c;

	memset(&m, 0, sizeof(m));
	memset(ctl, 0, sizeof(ctl));
	iov.iov_base = &b;
	iov.iov_len = 1;
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = ctl;
	m.msg_controllen = sizeof(ctl);
	c = CMSG_FIRSTHDR(&m);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &fd, sizeof(int));
	return (sendmsg(sock, &m, 0) == 1 ? 0 : -1);
}

static int
recv_fd(int sock)
{
	struct msghdr m;
	struct iovec iov;
	char b, ctl[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *c;
	int fd = -1;

	memset(&m, 0, sizeof(m));
	iov.iov_base = &b;
	iov.iov_len = 1;
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = ctl;
	m.msg_controllen = sizeof(ctl);
	if (recvmsg(sock, &m, 0) != 1)
		return (-1);
	c = CMSG_FIRSTHDR(&m);
	if (c != NULL && c->cmsg_type == SCM_RIGHTS)
		memcpy(&fd, CMSG_DATA(c), sizeof(int));
	return (fd);
}

struct peer {
	NvU32	client;
	int	ctl;
};

/* user B, in the child: allocate, send the handle and fd number, receive A's, then attack */
static void
run_b(const char *ub, int to_a, int from_a, int sock)
{
	struct side b;
	struct peer mine, theirs;

	if (become(ub, 65533) != 0 || setup(&b) != 0)
		_exit(2);
	mine.client = b.client;
	mine.ctl = b.ctl;
	if (write(to_a, &mine, sizeof(mine)) != sizeof(mine) || read(from_a, &theirs, sizeof(theirs)) != sizeof(theirs))
		_exit(2);
	if (send_fd(sock, b.ctl) != 0)
		_exit(2);
	if (!json)
		printf("B uid %u: client 0x%x; A's client 0x%x\n", (unsigned)getuid(), b.client, theirs.client);
	attack("B", &b, theirs.client, recv_fd(sock));
	if (!json)
		printf("B: %d rows, %d failed\n", rows, fails);
	else
		printf("{\"schema\":\"uvm-owner-probe/1\",\"who\":\"B\",\"summary\":true,\"rows\":%d,\"failed\":%d}\n", rows, fails);
	fflush(stdout);
	_exit(fails != 0);
}

/* user A, in the parent: the same, against B's handle */
static int
run_a(const char *ua, pid_t pid, int from_b, int to_b, int sock)
{
	struct side a;
	struct peer mine, theirs;
	int status;

	if (become(ua, 65534) != 0 || setup(&a) != 0) {
		kill(pid, 9);
		return (2);
	}
	mine.client = a.client;
	mine.ctl = a.ctl;
	if (read(from_b, &theirs, sizeof(theirs)) != sizeof(theirs) || write(to_b, &mine, sizeof(mine)) != sizeof(mine)) {
		kill(pid, 9);
		return (2);
	}
	if (send_fd(sock, a.ctl) != 0) {
		kill(pid, 9);
		return (2);
	}
	attack("A", &a, theirs.client, recv_fd(sock));
	if (!json)
		printf("A: %d rows, %d failed\n", rows, fails);
	else
		printf("{\"schema\":\"uvm-owner-probe/1\",\"who\":\"A\",\"summary\":true,\"rows\":%d,\"failed\":%d}\n", rows, fails);
	if (waitpid(pid, &status, 0) < 0)
		return (2);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("B's run failed (status 0x%x)\n", status);
		return (1);
	}
	return (fails != 0);
}

int
main(int argc, char **argv)
{
	const char *ua = NULL, *ub = NULL;
	int pa[2], pb[2], sp[2], c;
	pid_t pid;

	while ((c = getopt(argc, argv, "vja:b:")) != -1) {
		if (c == 'v')
			verbose = 1;
		else if (c == 'j')
			json = 1;
		else if (c == 'a')
			ua = optarg;
		else if (c == 'b')
			ub = optarg;
		else {
			printf("usage: uvm_owner_probe [-v] [-j (one JSON object per line)] [-a uidA] [-b uidB]\n");
			return (2);
		}
	}
	if (ua != NULL && ub != NULL && strcmp(ua, ub) == 0) {
		printf("-a and -b must differ: the probe needs two users\n");
		return (2);
	}
	if (geteuid() != 0) {
		printf("run as root: the two users are made with setuid\n");
		return (2);
	}
	if (pipe(pa) != 0 || pipe(pb) != 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0)
		return (2);
	fflush(stdout);
	pid = fork();
	if (pid < 0)
		return (2);
	if (pid == 0) {
		close(pa[0]);
		close(pb[1]);
		close(sp[0]);
		run_b(ub, pa[1], pb[0], sp[1]);
	}
	close(pa[1]);
	close(pb[0]);
	close(sp[1]);
	return (run_a(ua, pid, pa[0], pb[1], sp[0]));
}
