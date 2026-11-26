/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_ioctl_test.c - the userland harness of uvm-spec r1, 10-test-plan.md
 * section 0 and the section 1 table ("Module and device nodes"), plus the
 * fd and range checks of 01 sections 3 and 5 that need no GPU.  Every
 * expectation is a stated value from the spec, so a wrong driver fails.
 *
 * Builds natively (the driver's own request numbers, wrapped in
 * UVM_IOCTL_NATIVE) and as a Linux binary for the Linuxulator (the Linux
 * request numbers, which reach the driver through the optional bridge
 * module nvidia_uvm_linux.ko):
 *   cc -I sys -o uvm_ioctl_test tests/ioctl/uvm_ioctl_test.c
 *   (on Linux) cc -I sys -o uvm_ioctl_test_linux tests/ioctl/uvm_ioctl_test.c
 * Run as an ordinary user with nvidia.ko and nvidia-uvm.ko loaded:
 *   ./uvm_ioctl_test [-v]
 * Needs no CUDA and no GPU registration.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvidia_uvm_ioctl.h"
#include "../../linux/nvidia_uvm_linux_ioctl.h"

/*
 * The request word the harness sends.
 *
 * Natively the harness speaks the driver's own request numbers, wrapped in
 * the native encoding of nvidia_uvm_ioctl.h.  Built for Linux it speaks the
 * Linux request numbers, which reach the driver only through the optional
 * Linux ioctl bridge (nvidia_uvm_linux.ko).
 */
#ifdef __FreeBSD__
#define	UVM_TEST_REQUEST_WORD(id)	UVM_IOCTL_NATIVE(id)
#define	UVM_TEST_UNKNOWN_REQUEST	UVM_REQ_NONE
#else
#define	UVM_TEST_REQUEST_WORD(id)	((unsigned long)uvm_linux_request_number(id))
#define	UVM_TEST_UNKNOWN_REQUEST	LINUX_UVM_IS_8_SUPPORTED
#endif

static int verbose;
static int fails, passes;

#define	CHECK(cond, ...) do {						\
	if (cond) {							\
		passes++;						\
		if (verbose) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
	} else {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf(" (errno %d %s)\n", errno, strerror(errno));	\
	}								\
} while (0)

static int
uvm_ioctl(int fd, uint32_t request, void *params)
{
	/*
	 * errno is cleared first so that the errno a failing check prints is
	 * this call's own: a call that succeeds leaves errno at zero, and a
	 * stale value from an earlier failure cannot be read as this one's.
	 */
	errno = 0;
	return (ioctl(fd, UVM_TEST_REQUEST_WORD(request), params));
}

static int
open_main(void)
{
	int fd = open("/dev/nvidia-uvm", O_RDWR);

	if (fd < 0) {
		printf("cannot open /dev/nvidia-uvm: %s\n", strerror(errno));
		exit(2);
	}
	return (fd);
}

static int
initialize(int fd, NvU64 flags, NV_STATUS *status)
{
	UVM_INITIALIZE_PARAMS p;
	int r;

	memset(&p, 0, sizeof(p));
	p.flags = flags;
	r = uvm_ioctl(fd, UVM_REQ_INITIALIZE, &p);
	*status = p.rmStatus;
	return (r);
}

static void
test_nodes_and_unknown(void)
{
	int fd, tfd, r;
	UVM_REGISTER_GPU_PARAMS rg;

	fd = open_main();
	tfd = open("/dev/nvidia-uvm-tools", O_RDWR);
	CHECK(tfd >= 0, "open /dev/nvidia-uvm-tools");

	/*
	 * A request the driver does not have: ENOSYS on the main node.
	 * Natively that is the reserved number UVM_REQ_NONE (the bridge sends
	 * it for a Linux request number the driver has no request for); built
	 * for Linux it is the Linux request number 2047, which the bridge
	 * turns into the same reserved number.
	 */
	errno = 0;
	r = uvm_ioctl(fd, UVM_TEST_UNKNOWN_REQUEST, &rg);
	CHECK(r == -1 && errno == ENOSYS, "a request the driver does not have fails with ENOSYS");
#ifdef __FreeBSD__
	/* an id past the last request: ENOSYS as well */
	errno = 0;
	r = uvm_ioctl(fd, UVM_REQ_COUNT, &rg);
	CHECK(r == -1 && errno == ENOSYS, "a request id past the last one fails with ENOSYS");
#endif
	memset(&rg, 0, sizeof(rg));
	errno = 0;
	r = uvm_ioctl(tfd, UVM_REQ_REGISTER_GPU, &rg);
	CHECK(r == -1 && errno == EINVAL, "a request of the main node fails with EINVAL on the tools node");
	{
		UVM_IS_8_SUPPORTED_PARAMS p8;

		memset(&p8, 0, sizeof(p8));
		errno = 0;
		r = uvm_ioctl(fd, UVM_TEST_UNKNOWN_REQUEST, &p8);
		CHECK(r == -1 && errno == ENOSYS, "a request the driver does not have fails with ENOSYS");
	}
	/* a bad parameter pointer: EFAULT */
	errno = 0;
	r = uvm_ioctl(fd, UVM_REQ_PAGEABLE_MEM_ACCESS, (void *)(uintptr_t)0x10);
	CHECK(r == -1 && errno == EFAULT, "unreadable parameter memory fails with EFAULT");
	/* DEINITIALIZE on an uninitialised fd: 0 */
	r = uvm_ioctl(fd, UVM_REQ_DEINITIALIZE, NULL);
	CHECK(r == 0, "UVM_REQ_DEINITIALIZE on an uninitialised fd returns 0");
	if (tfd >= 0)
		close(tfd);
	close(fd);
}

static void
test_fd_states(void)
{
	UVM_PAGEABLE_MEM_ACCESS_PARAMS pma;
	UVM_MM_INITIALIZE_PARAMS mm;
	NV_STATUS st;
	int fd, fd2, r;

	fd = open_main();
	/* a VA-space ioctl before UVM_REQ_INITIALIZE: syscall 0, rmStatus NV_ERR_ILLEGAL_ACTION */
	memset(&pma, 0, sizeof(pma));
	r = uvm_ioctl(fd, UVM_REQ_PAGEABLE_MEM_ACCESS, &pma);
	CHECK(r == 0 && pma.rmStatus == NV_ERR_ILLEGAL_ACTION, "ioctl before INITIALIZE: rmStatus ILLEGAL_ACTION, r=%d st=%#x",
	    r, pma.rmStatus);
	/* an invalid flag */
	r = initialize(fd, 1ULL << 5, &st);
	CHECK(r == 0 && st == NV_ERR_INVALID_ARGUMENT, "INITIALIZE with an unknown flag: INVALID_ARGUMENT (%#x)", st);
	/* then a good one */
	r = initialize(fd, UVM_INIT_FLAGS_DISABLE_HMM, &st);
	CHECK(r == 0 && st == NV_OK, "INITIALIZE: NV_OK (%#x)", st);
	/* twice: same flags NV_OK, different flags INVALID_ARGUMENT */
	r = initialize(fd, UVM_INIT_FLAGS_DISABLE_HMM, &st);
	CHECK(r == 0 && st == NV_OK, "INITIALIZE again, same flags: NV_OK (%#x)", st);
	r = initialize(fd, 0, &st);
	CHECK(r == 0 && st == NV_ERR_INVALID_ARGUMENT, "INITIALIZE again, other flags: INVALID_ARGUMENT (%#x)", st);
	/* pageable memory access: 0 */
	memset(&pma, 0xff, sizeof(pma));
	r = uvm_ioctl(fd, UVM_REQ_PAGEABLE_MEM_ACCESS, &pma);
	CHECK(r == 0 && pma.rmStatus == NV_OK && pma.pageableMemAccess == 0, "PAGEABLE_MEM_ACCESS is 0");

	/* MM_INITIALIZE: a second fd bound to the first; this platform answers NOTHING_TO_DO or NV_OK */
	fd2 = open_main();
	memset(&mm, 0, sizeof(mm));
	mm.uvmFd = fd;
	r = uvm_ioctl(fd2, UVM_REQ_MM_INITIALIZE, &mm);
	CHECK(r == 0 && mm.rmStatus == NV_OK, "MM_INITIALIZE on a VA-space fd: NV_OK (%#x)", mm.rmStatus);
	/* the fd is MM now: asking again on it, or on the VA-space fd itself, is IN_USE (01 section 3.4) */
	mm.uvmFd = fd;
	r = uvm_ioctl(fd2, UVM_REQ_MM_INITIALIZE, &mm);
	CHECK(r == 0 && mm.rmStatus == NV_ERR_IN_USE, "MM_INITIALIZE on an MM fd: IN_USE (%#x)", mm.rmStatus);
	r = uvm_ioctl(fd, UVM_REQ_MM_INITIALIZE, &mm);
	CHECK(r == 0 && mm.rmStatus == NV_ERR_IN_USE, "MM_INITIALIZE on a VA-space fd: IN_USE (%#x)", mm.rmStatus);
	/* the binding is ALIVE: a second MM fd for the same VA space is INVALID_STATE */
	{
		int fd4 = open_main();

		r = uvm_ioctl(fd4, UVM_REQ_MM_INITIALIZE, &mm);
		CHECK(r == 0 && mm.rmStatus == NV_ERR_INVALID_STATE, "second MM fd: INVALID_STATE (%#x)", mm.rmStatus);
		close(fd4);
	}
	/* an MM fd accepts no VA-space ioctl */
	memset(&pma, 0, sizeof(pma));
	r = uvm_ioctl(fd2, UVM_REQ_PAGEABLE_MEM_ACCESS, &pma);
	CHECK(r == 0 && pma.rmStatus == NV_ERR_ILLEGAL_ACTION, "VA-space ioctl on an MM fd: ILLEGAL_ACTION (%#x)",
	    pma.rmStatus);
	/* with a bad fd */
	mm.uvmFd = -1;
	r = uvm_ioctl(fd2, UVM_REQ_MM_INITIALIZE, &mm);
	CHECK(r == 0 && mm.rmStatus == NV_ERR_INVALID_ARGUMENT, "MM_INITIALIZE with a closed fd: INVALID_ARGUMENT (%#x)",
	    mm.rmStatus);
	/* with an uninitialised UVM fd */
	{
		int fd3 = open_main();

		mm.uvmFd = fd3;
		r = uvm_ioctl(fd2, UVM_REQ_MM_INITIALIZE, &mm);
		CHECK(r == 0 && mm.rmStatus == NV_ERR_INVALID_ARGUMENT,
		    "MM_INITIALIZE naming an uninitialised fd: INVALID_ARGUMENT (%#x)", mm.rmStatus);
		close(fd3);
	}
	close(fd2);
	close(fd);
}

/* #lizard forgives -- the range table of the test plan, one check per row */
static void
test_ranges(void)
{
	UVM_CREATE_EXTERNAL_RANGE_PARAMS cr;
	UVM_VALIDATE_VA_RANGE_PARAMS vr;
	UVM_FREE_PARAMS fr;
	UVM_SET_PREFERRED_LOCATION_PARAMS pl;
	UVM_MIGRATE_PARAMS mig;
	UVM_CREATE_RANGE_GROUP_PARAMS cg;
	UVM_DESTROY_RANGE_GROUP_PARAMS dg;
	UVM_PREVENT_MIGRATION_RANGE_GROUPS_PARAMS pm;
	UVM_REGISTER_GPU_PARAMS rg;
	UVM_ALLOC_DEVICE_P2P_PARAMS p2p;
	UVM_CLEAN_UP_ZOMBIE_RESOURCES_PARAMS zr;
	UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_V2_PARAMS ut;
	NvProcessorUuid table[UVM_MAX_GPUS + 1];
	NV_STATUS st;
	const NvU64 base = 0x7f0000000000ULL;	/* a canonical user address well away from the heap */
	int fd, r;
	void *m, *managed_left_mapped = NULL;

	fd = open_main();
	r = initialize(fd, 0, &st);
	CHECK(r == 0 && st == NV_OK, "INITIALIZE for the range tests");

	/* external range: alignment, collision, validate, free */
	memset(&cr, 0, sizeof(cr));
	cr.base = base + 0x800;
	cr.length = 0x1000;
	r = uvm_ioctl(fd, UVM_REQ_CREATE_EXTERNAL_RANGE, &cr);
	CHECK(r == 0 && cr.rmStatus == NV_ERR_INVALID_ADDRESS, "external range: unaligned base INVALID_ADDRESS (%#x)",
	    cr.rmStatus);
	cr.base = base;
	cr.length = 0;
	r = uvm_ioctl(fd, UVM_REQ_CREATE_EXTERNAL_RANGE, &cr);
	CHECK(r == 0 && cr.rmStatus == NV_ERR_INVALID_ADDRESS, "external range: zero length INVALID_ADDRESS");
	cr.length = 0x10000;
	r = uvm_ioctl(fd, UVM_REQ_CREATE_EXTERNAL_RANGE, &cr);
	CHECK(r == 0 && cr.rmStatus == NV_OK, "external range created (%#x)", cr.rmStatus);
	cr.base = base + 0x8000;
	r = uvm_ioctl(fd, UVM_REQ_CREATE_EXTERNAL_RANGE, &cr);
	CHECK(r == 0 && cr.rmStatus == NV_ERR_UVM_ADDRESS_IN_USE, "overlapping external range: ADDRESS_IN_USE (%#x)",
	    cr.rmStatus);
	memset(&vr, 0, sizeof(vr));
	vr.base = base;
	vr.length = 0x10000;
	r = uvm_ioctl(fd, UVM_REQ_VALIDATE_VA_RANGE, &vr);
	CHECK(r == 0 && vr.rmStatus == NV_OK, "VALIDATE_VA_RANGE of the exact range: NV_OK");
	vr.length = 0x8000;
	r = uvm_ioctl(fd, UVM_REQ_VALIDATE_VA_RANGE, &vr);
	CHECK(r == 0 && vr.rmStatus == NV_ERR_INVALID_ADDRESS, "VALIDATE_VA_RANGE of a part: INVALID_ADDRESS");

	/* mmap of the external range collides (ADDRINUSE); of a free address is a later phase (ENOSYS) */
	m = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)base);
	CHECK(m == MAP_FAILED && errno == EADDRINUSE, "mmap over the external range: EADDRINUSE");
	/* a private mapping or one without read/write is refused before anything is created */
	m = mmap(NULL, 0x10000, PROT_READ, MAP_SHARED, fd, (off_t)(base + 0x100000));
	CHECK(m == MAP_FAILED && errno == EINVAL, "mmap read-only of a free address: EINVAL (%d)", errno);
	/* a managed range: created by the mmap, found by the policy ioctls, gone with the mapping */
	m = mmap((void *)(base + 0x100000), 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_EXCL, fd,
	    (off_t)(base + 0x100000));
	CHECK(m == (void *)(base + 0x100000), "mmap of a free address creates a managed range (%d)", errno);
	if (m != MAP_FAILED) {
		volatile unsigned *w = m;

		CHECK(w[0] == 0 && w[0x3fff] == 0, "managed memory starts zeroed");
		w[0x10] = 0x1234;
		CHECK(w[0x10] == 0x1234, "the CPU reads back what it wrote");
		vr.base = base + 0x100000;
		vr.length = 0x10000;
		r = uvm_ioctl(fd, UVM_REQ_VALIDATE_VA_RANGE, &vr);
		CHECK(r == 0 && vr.rmStatus == NV_OK, "VALIDATE_VA_RANGE of the managed range: NV_OK (%#x)", vr.rmStatus);
		memset(&pl, 0, sizeof(pl));
		pl.requestedBase = base + 0x101000;
		pl.length = 0x2000;
		pl.preferredLocation = NV_PROCESSOR_UUID_CPU_DEFAULT;
		pl.preferredCpuNumaNode = -1;
		r = uvm_ioctl(fd, UVM_REQ_SET_PREFERRED_LOCATION, &pl);
		CHECK(r == 0 && pl.rmStatus == NV_OK, "SET_PREFERRED_LOCATION inside a managed range: NV_OK (%#x)",
		    pl.rmStatus);
		pl.requestedBase = base + 0x10f000;
		pl.length = 0x2000;	/* runs past the end of the range: a gap */
		r = uvm_ioctl(fd, UVM_REQ_SET_PREFERRED_LOCATION, &pl);
		CHECK(r == 0 && pl.rmStatus == NV_ERR_INVALID_ADDRESS, "SET_PREFERRED_LOCATION across the end: INVALID_ADDRESS (%#x)",
		    pl.rmStatus);
		memset(&mig, 0, sizeof(mig));
		mig.base = base + 0x100000;
		mig.length = 0x10000;
		mig.destinationUuid = NV_PROCESSOR_UUID_CPU_DEFAULT;
		mig.cpuNumaNode = -1;
		r = uvm_ioctl(fd, UVM_REQ_MIGRATE, &mig);
		CHECK(r == 0 && mig.rmStatus == NV_OK, "MIGRATE of a managed interval to the CPU: NV_OK (%#x)", mig.rmStatus);
		/* a second mapping over the same addresses collides; UVM_REQ_FREE does not free managed memory */
		{
			void *m2 = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)(base + 0x104000));

			CHECK(m2 == MAP_FAILED && errno == EADDRINUSE, "mmap inside a managed range: EADDRINUSE (%d)", errno);
		}
		memset(&fr, 0, sizeof(fr));
		fr.base = base + 0x100000;
		fr.length = 0x10000;
		r = uvm_ioctl(fd, UVM_REQ_FREE, &fr);
		CHECK(r == 0 && fr.rmStatus == NV_ERR_INVALID_ADDRESS, "UVM_REQ_FREE of a managed range: INVALID_ADDRESS (%#x)",
		    fr.rmStatus);
		CHECK(munmap(m, 0x10000) == 0, "munmap of the managed range");
		r = uvm_ioctl(fd, UVM_REQ_VALIDATE_VA_RANGE, &vr);
		CHECK(r == 0 && vr.rmStatus == NV_ERR_INVALID_ADDRESS, "VALIDATE after munmap: INVALID_ADDRESS (%#x)", vr.rmStatus);
		/* the range is free again: a new mapping works, and survives the close of the fd (detached object) */
		m = mmap((void *)(base + 0x100000), 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_EXCL, fd,
		    (off_t)(base + 0x100000));
		CHECK(m == (void *)(base + 0x100000), "the address is reusable after munmap (%d)", errno);
		if (m != MAP_FAILED) {
			volatile unsigned *w2 = m;

			w2[1] = 77;
			managed_left_mapped = m;
		}
	}

	/* policy and migration on a non-managed interval: INVALID_ADDRESS */
	memset(&pl, 0, sizeof(pl));
	pl.requestedBase = base;
	pl.length = 0x10000;
	pl.preferredLocation = NV_PROCESSOR_UUID_CPU_DEFAULT;
	pl.preferredCpuNumaNode = -1;
	r = uvm_ioctl(fd, UVM_REQ_SET_PREFERRED_LOCATION, &pl);
	CHECK(r == 0 && pl.rmStatus == NV_ERR_INVALID_ADDRESS, "SET_PREFERRED_LOCATION on an external range: INVALID_ADDRESS (%#x)",
	    pl.rmStatus);
	memset(&mig, 0, sizeof(mig));
	mig.base = base;
	mig.length = 0x10000;
	mig.destinationUuid = NV_PROCESSOR_UUID_CPU_DEFAULT;
	mig.cpuNumaNode = -1;
	mig.flags = 1u << 1;	/* a test-only flag */
	r = uvm_ioctl(fd, UVM_REQ_MIGRATE, &mig);
	CHECK(r == 0 && mig.rmStatus == NV_ERR_INVALID_ARGUMENT, "MIGRATE with a test flag: INVALID_ARGUMENT (%#x)", mig.rmStatus);
	mig.flags = 0;
	mig.semaphoreAddress = 0x1000;
	r = uvm_ioctl(fd, UVM_REQ_MIGRATE, &mig);
	CHECK(r == 0 && mig.rmStatus == NV_ERR_INVALID_ARGUMENT, "synchronous MIGRATE with a semaphore: INVALID_ARGUMENT");
	mig.semaphoreAddress = 0;
	r = uvm_ioctl(fd, UVM_REQ_MIGRATE, &mig);
	CHECK(r == 0 && mig.rmStatus == NV_ERR_INVALID_ADDRESS, "MIGRATE of a non-managed interval: INVALID_ADDRESS (%#x)",
	    mig.rmStatus);

	/* range groups: ids from 1, unknown ids OBJECT_NOT_FOUND, count limit */
	memset(&cg, 0, sizeof(cg));
	r = uvm_ioctl(fd, UVM_REQ_CREATE_RANGE_GROUP, &cg);
	CHECK(r == 0 && cg.rmStatus == NV_OK && cg.rangeGroupId == 1, "first range group id is 1 (%ju)",
	    (uintmax_t)cg.rangeGroupId);
	memset(&pm, 0, sizeof(pm));
	pm.numGroupIds = 33;
	r = uvm_ioctl(fd, UVM_REQ_PREVENT_MIGRATION_RANGE_GROUPS, &pm);
	CHECK(r == 0 && pm.rmStatus == NV_ERR_INVALID_ARGUMENT, "PREVENT_MIGRATION with 33 ids: INVALID_ARGUMENT");
	pm.numGroupIds = 1;
	pm.rangeGroupIds[0] = 77;
	r = uvm_ioctl(fd, UVM_REQ_PREVENT_MIGRATION_RANGE_GROUPS, &pm);
	CHECK(r == 0 && pm.rmStatus == NV_ERR_OBJECT_NOT_FOUND, "PREVENT_MIGRATION of an unknown group: OBJECT_NOT_FOUND");
	pm.rangeGroupIds[0] = cg.rangeGroupId;
	r = uvm_ioctl(fd, UVM_REQ_PREVENT_MIGRATION_RANGE_GROUPS, &pm);
	CHECK(r == 0 && pm.rmStatus == NV_OK, "PREVENT_MIGRATION of an empty group: NV_OK");
	memset(&dg, 0, sizeof(dg));
	dg.rangeGroupId = 77;
	r = uvm_ioctl(fd, UVM_REQ_DESTROY_RANGE_GROUP, &dg);
	CHECK(r == 0 && dg.rmStatus == NV_ERR_OBJECT_NOT_FOUND, "DESTROY of an unknown group: OBJECT_NOT_FOUND");
	dg.rangeGroupId = cg.rangeGroupId;
	r = uvm_ioctl(fd, UVM_REQ_DESTROY_RANGE_GROUP, &dg);
	CHECK(r == 0 && dg.rmStatus == NV_OK, "DESTROY of the group: NV_OK");

	/* an unknown GPU UUID */
	memset(&rg, 0, sizeof(rg));
	memset(rg.gpu_uuid.uuid, 0xa5, sizeof(rg.gpu_uuid.uuid));
	r = uvm_ioctl(fd, UVM_REQ_REGISTER_GPU, &rg);
	CHECK(r == 0 && rg.rmStatus == NV_ERR_GPU_UUID_NOT_FOUND, "REGISTER_GPU with an unknown UUID: GPU_UUID_NOT_FOUND (%#x)",
	    rg.rmStatus);
	/* device P2P: not supported on this platform */
	memset(&p2p, 0, sizeof(p2p));
	r = uvm_ioctl(fd, UVM_REQ_ALLOC_DEVICE_P2P, &p2p);
	CHECK(r == 0 && p2p.rmStatus == NV_ERR_NOT_SUPPORTED, "ALLOC_DEVICE_P2P: NOT_SUPPORTED (%#x)", p2p.rmStatus);
	/* zombie cleanup always NV_OK */
	memset(&zr, 0xff, sizeof(zr));
	r = uvm_ioctl(fd, UVM_REQ_CLEAN_UP_ZOMBIE_RESOURCES, &zr);
	CHECK(r == 0 && zr.rmStatus == NV_OK, "CLEAN_UP_ZOMBIE_RESOURCES: NV_OK");
	/* the UUID table: entry 0 is the CPU, the rest zero without GPUs */
	memset(&ut, 0, sizeof(ut));
	memset(table, 0xee, sizeof(table));
	ut.tablePtr = (NvU64)(uintptr_t)table;
	r = uvm_ioctl(fd, UVM_REQ_TOOLS_GET_PROCESSOR_UUID_TABLE_V2, &ut);
	CHECK(r == 0 && ut.rmStatus == NV_OK && memcmp(table[0].uuid, NV_PROCESSOR_UUID_CPU_DEFAULT.uuid, 16) == 0 &&
	    table[1].uuid[0] == 0 && table[256].uuid[15] == 0, "UUID table V2: CPU at 0, GPUs zero");
	ut.tablePtr = 0x10;
	r = uvm_ioctl(fd, UVM_REQ_TOOLS_GET_PROCESSOR_UUID_TABLE_V2, &ut);
	CHECK(r == 0 && ut.rmStatus == NV_ERR_INVALID_ADDRESS, "UUID table to a bad pointer: INVALID_ADDRESS");

	/* free: wrong length INVALID_ADDRESS, then the exact range */
	memset(&fr, 0, sizeof(fr));
	fr.base = base;
	fr.length = 0x8000;
	r = uvm_ioctl(fd, UVM_REQ_FREE, &fr);
	CHECK(r == 0 && fr.rmStatus == NV_ERR_INVALID_ADDRESS, "FREE with the wrong length: INVALID_ADDRESS");
	fr.length = 0x10000;
	r = uvm_ioctl(fd, UVM_REQ_FREE, &fr);
	CHECK(r == 0 && fr.rmStatus == NV_OK, "FREE of the external range: NV_OK");
	vr.base = base;
	vr.length = 0x10000;
	r = uvm_ioctl(fd, UVM_REQ_VALIDATE_VA_RANGE, &vr);
	CHECK(r == 0 && vr.rmStatus == NV_ERR_INVALID_ADDRESS, "VALIDATE after FREE: INVALID_ADDRESS");
	close(fd);
	if (managed_left_mapped != NULL) {
		/* the VA space is gone; the mapping stays readable and writable (detached pager object), then unmaps */
		volatile unsigned *w = managed_left_mapped;

		CHECK(w[1] == 77, "a managed mapping outlives the closed fd and keeps its data");
		w[2] = 5;
		CHECK(munmap(managed_left_mapped, 0x10000) == 0, "munmap after the VA space is gone");
	}
}

/* open/close many times, with forks: no leak the kernel's counters would show (vmstat -m nvidia_uvm) */
static void
test_churn(void)
{
	int i, fd, r;
	NV_STATUS st;
	pid_t pid;

	for (i = 0; i < 2000; i++) {
		fd = open_main();
		if (i % 4 == 0) {
			r = initialize(fd, 0, &st);
			if (r != 0 || st != NV_OK) {
				fails++;
				printf("FAIL churn: INITIALIZE %d %#x\n", r, st);
				close(fd);
				return;
			}
		}
		close(fd);
	}
	passes++;
	for (i = 0; i < 8; i++) {
		fd = open_main();
		r = initialize(fd, 0, &st);
		pid = fork();
		if (pid == 0) {
			/* the child inherits the fd; it must be able to close it and exit */
			close(fd);
			_exit(0);
		}
		CHECK(pid > 0, "fork");
		if (pid > 0) {
			int status;

			waitpid(pid, &status, 0);
			CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exited 0");
		}
		close(fd);
	}
}

int
main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "-v") == 0)
		verbose = 1;
	test_nodes_and_unknown();
	test_fd_states();
	test_ranges();
	test_churn();
	printf("uvm_ioctl_test: %d passed, %d failed\n", passes, fails);
	return (fails != 0);
}
