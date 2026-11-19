/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_ioctl_test.c - the userland harness of uvm-spec r1, 10-test-plan.md
 * section 0 and the section 1 table ("Module and device nodes"), plus the
 * fd and range checks of 01 sections 3 and 5 that need no GPU.  Every
 * expectation is a stated value from the spec, so a wrong driver fails.
 *
 * Builds natively (the request numbers wrapped in UVM_IOCTL_NATIVE) and as
 * a Linux binary for the Linuxulator (raw numbers):
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
uvm_ioctl(int fd, unsigned long raw, void *params)
{
	return (ioctl(fd, UVM_IOCTL_NATIVE(raw), params));
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
	r = uvm_ioctl(fd, UVM_INITIALIZE, &p);
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

	/* unknown number 1000 on the main node: ENOSYS; 37 on the tools node: EINVAL */
	errno = 0;
	r = uvm_ioctl(fd, 1000, &rg);
	CHECK(r == -1 && errno == ENOSYS, "unknown ioctl 1000 fails with ENOSYS");
	memset(&rg, 0, sizeof(rg));
	errno = 0;
	r = uvm_ioctl(tfd, UVM_REGISTER_GPU, &rg);
	CHECK(r == -1 && errno == EINVAL, "ioctl 37 on the tools node fails with EINVAL");
	/* retired numbers 1-7, 9, 10-22, 35 and 2047: ENOSYS */
	errno = 0;
	r = uvm_ioctl(fd, 9, &rg);
	CHECK(r == -1 && errno == ENOSYS, "retired ioctl 9 fails with ENOSYS");
	errno = 0;
	r = uvm_ioctl(fd, 35, &rg);
	CHECK(r == -1 && errno == ENOSYS, "retired ioctl 35 fails with ENOSYS");
	{
		UVM_IS_8_SUPPORTED_PARAMS p8;

		memset(&p8, 0, sizeof(p8));
		errno = 0;
		r = uvm_ioctl(fd, UVM_IS_8_SUPPORTED, &p8);
		CHECK(r == -1 && errno == ENOSYS, "UVM_IS_8_SUPPORTED (2047) fails with ENOSYS");
	}
	/* a bad parameter pointer: EFAULT */
	errno = 0;
	r = uvm_ioctl(fd, UVM_PAGEABLE_MEM_ACCESS, (void *)(uintptr_t)0x10);
	CHECK(r == -1 && errno == EFAULT, "unreadable parameter memory fails with EFAULT");
	/* DEINITIALIZE on an uninitialised fd: 0 */
	r = uvm_ioctl(fd, UVM_DEINITIALIZE, NULL);
	CHECK(r == 0, "UVM_DEINITIALIZE on an uninitialised fd returns 0");
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
	/* a VA-space ioctl before UVM_INITIALIZE: syscall 0, rmStatus NV_ERR_ILLEGAL_ACTION */
	memset(&pma, 0, sizeof(pma));
	r = uvm_ioctl(fd, UVM_PAGEABLE_MEM_ACCESS, &pma);
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
	r = uvm_ioctl(fd, UVM_PAGEABLE_MEM_ACCESS, &pma);
	CHECK(r == 0 && pma.rmStatus == NV_OK && pma.pageableMemAccess == 0, "PAGEABLE_MEM_ACCESS is 0");

	/* MM_INITIALIZE: a second fd bound to the first; this platform answers NOTHING_TO_DO or NV_OK */
	fd2 = open_main();
	memset(&mm, 0, sizeof(mm));
	mm.uvmFd = fd;
	r = uvm_ioctl(fd2, UVM_MM_INITIALIZE, &mm);
	CHECK(r == 0 && (mm.rmStatus == NV_WARN_NOTHING_TO_DO || mm.rmStatus == NV_OK),
	    "MM_INITIALIZE on a VA-space fd: NOTHING_TO_DO or NV_OK (%#x)", mm.rmStatus);
	/* with a bad fd */
	mm.uvmFd = -1;
	r = uvm_ioctl(fd2, UVM_MM_INITIALIZE, &mm);
	CHECK(r == 0 && mm.rmStatus == NV_ERR_INVALID_ARGUMENT, "MM_INITIALIZE with a closed fd: INVALID_ARGUMENT (%#x)",
	    mm.rmStatus);
	/* with an uninitialised UVM fd */
	{
		int fd3 = open_main();

		mm.uvmFd = fd3;
		r = uvm_ioctl(fd2, UVM_MM_INITIALIZE, &mm);
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
	void *m;

	fd = open_main();
	r = initialize(fd, 0, &st);
	CHECK(r == 0 && st == NV_OK, "INITIALIZE for the range tests");

	/* external range: alignment, collision, validate, free */
	memset(&cr, 0, sizeof(cr));
	cr.base = base + 0x800;
	cr.length = 0x1000;
	r = uvm_ioctl(fd, UVM_CREATE_EXTERNAL_RANGE, &cr);
	CHECK(r == 0 && cr.rmStatus == NV_ERR_INVALID_ADDRESS, "external range: unaligned base INVALID_ADDRESS (%#x)",
	    cr.rmStatus);
	cr.base = base;
	cr.length = 0;
	r = uvm_ioctl(fd, UVM_CREATE_EXTERNAL_RANGE, &cr);
	CHECK(r == 0 && cr.rmStatus == NV_ERR_INVALID_ADDRESS, "external range: zero length INVALID_ADDRESS");
	cr.length = 0x10000;
	r = uvm_ioctl(fd, UVM_CREATE_EXTERNAL_RANGE, &cr);
	CHECK(r == 0 && cr.rmStatus == NV_OK, "external range created (%#x)", cr.rmStatus);
	cr.base = base + 0x8000;
	r = uvm_ioctl(fd, UVM_CREATE_EXTERNAL_RANGE, &cr);
	CHECK(r == 0 && cr.rmStatus == NV_ERR_UVM_ADDRESS_IN_USE, "overlapping external range: ADDRESS_IN_USE (%#x)",
	    cr.rmStatus);
	memset(&vr, 0, sizeof(vr));
	vr.base = base;
	vr.length = 0x10000;
	r = uvm_ioctl(fd, UVM_VALIDATE_VA_RANGE, &vr);
	CHECK(r == 0 && vr.rmStatus == NV_OK, "VALIDATE_VA_RANGE of the exact range: NV_OK");
	vr.length = 0x8000;
	r = uvm_ioctl(fd, UVM_VALIDATE_VA_RANGE, &vr);
	CHECK(r == 0 && vr.rmStatus == NV_ERR_INVALID_ADDRESS, "VALIDATE_VA_RANGE of a part: INVALID_ADDRESS");

	/* mmap of the external range collides (ADDRINUSE); of a free address is a later phase (ENOSYS) */
	m = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)base);
	CHECK(m == MAP_FAILED && errno == EADDRINUSE, "mmap over the external range: EADDRINUSE");
	m = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)(base + 0x100000));
	CHECK(m == MAP_FAILED && (errno == ENOSYS || errno == EINVAL), "mmap of a free address: refused (%d)", errno);

	/* policy and migration on a non-managed interval: INVALID_ADDRESS */
	memset(&pl, 0, sizeof(pl));
	pl.requestedBase = base;
	pl.length = 0x10000;
	pl.preferredLocation = NV_PROCESSOR_UUID_CPU_DEFAULT;
	pl.preferredCpuNumaNode = -1;
	r = uvm_ioctl(fd, UVM_SET_PREFERRED_LOCATION, &pl);
	CHECK(r == 0 && pl.rmStatus == NV_ERR_INVALID_ADDRESS, "SET_PREFERRED_LOCATION on an external range: INVALID_ADDRESS (%#x)",
	    pl.rmStatus);
	memset(&mig, 0, sizeof(mig));
	mig.base = base;
	mig.length = 0x10000;
	mig.destinationUuid = NV_PROCESSOR_UUID_CPU_DEFAULT;
	mig.cpuNumaNode = -1;
	mig.flags = 1u << 1;	/* a test-only flag */
	r = uvm_ioctl(fd, UVM_MIGRATE, &mig);
	CHECK(r == 0 && mig.rmStatus == NV_ERR_INVALID_ARGUMENT, "MIGRATE with a test flag: INVALID_ARGUMENT (%#x)", mig.rmStatus);
	mig.flags = 0;
	mig.semaphoreAddress = 0x1000;
	r = uvm_ioctl(fd, UVM_MIGRATE, &mig);
	CHECK(r == 0 && mig.rmStatus == NV_ERR_INVALID_ARGUMENT, "synchronous MIGRATE with a semaphore: INVALID_ARGUMENT");
	mig.semaphoreAddress = 0;
	r = uvm_ioctl(fd, UVM_MIGRATE, &mig);
	CHECK(r == 0 && mig.rmStatus == NV_ERR_INVALID_ADDRESS, "MIGRATE of a non-managed interval: INVALID_ADDRESS (%#x)",
	    mig.rmStatus);

	/* range groups: ids from 1, unknown ids OBJECT_NOT_FOUND, count limit */
	memset(&cg, 0, sizeof(cg));
	r = uvm_ioctl(fd, UVM_CREATE_RANGE_GROUP, &cg);
	CHECK(r == 0 && cg.rmStatus == NV_OK && cg.rangeGroupId == 1, "first range group id is 1 (%ju)",
	    (uintmax_t)cg.rangeGroupId);
	memset(&pm, 0, sizeof(pm));
	pm.numGroupIds = 33;
	r = uvm_ioctl(fd, UVM_PREVENT_MIGRATION_RANGE_GROUPS, &pm);
	CHECK(r == 0 && pm.rmStatus == NV_ERR_INVALID_ARGUMENT, "PREVENT_MIGRATION with 33 ids: INVALID_ARGUMENT");
	pm.numGroupIds = 1;
	pm.rangeGroupIds[0] = 77;
	r = uvm_ioctl(fd, UVM_PREVENT_MIGRATION_RANGE_GROUPS, &pm);
	CHECK(r == 0 && pm.rmStatus == NV_ERR_OBJECT_NOT_FOUND, "PREVENT_MIGRATION of an unknown group: OBJECT_NOT_FOUND");
	pm.rangeGroupIds[0] = cg.rangeGroupId;
	r = uvm_ioctl(fd, UVM_PREVENT_MIGRATION_RANGE_GROUPS, &pm);
	CHECK(r == 0 && pm.rmStatus == NV_OK, "PREVENT_MIGRATION of an empty group: NV_OK");
	memset(&dg, 0, sizeof(dg));
	dg.rangeGroupId = 77;
	r = uvm_ioctl(fd, UVM_DESTROY_RANGE_GROUP, &dg);
	CHECK(r == 0 && dg.rmStatus == NV_ERR_OBJECT_NOT_FOUND, "DESTROY of an unknown group: OBJECT_NOT_FOUND");
	dg.rangeGroupId = cg.rangeGroupId;
	r = uvm_ioctl(fd, UVM_DESTROY_RANGE_GROUP, &dg);
	CHECK(r == 0 && dg.rmStatus == NV_OK, "DESTROY of the group: NV_OK");

	/* an unknown GPU UUID */
	memset(&rg, 0, sizeof(rg));
	memset(rg.gpu_uuid.uuid, 0xa5, sizeof(rg.gpu_uuid.uuid));
	r = uvm_ioctl(fd, UVM_REGISTER_GPU, &rg);
	CHECK(r == 0 && rg.rmStatus == NV_ERR_GPU_UUID_NOT_FOUND, "REGISTER_GPU with an unknown UUID: GPU_UUID_NOT_FOUND (%#x)",
	    rg.rmStatus);
	/* device P2P: not supported on this platform */
	memset(&p2p, 0, sizeof(p2p));
	r = uvm_ioctl(fd, UVM_ALLOC_DEVICE_P2P, &p2p);
	CHECK(r == 0 && p2p.rmStatus == NV_ERR_NOT_SUPPORTED, "ALLOC_DEVICE_P2P: NOT_SUPPORTED (%#x)", p2p.rmStatus);
	/* zombie cleanup always NV_OK */
	memset(&zr, 0xff, sizeof(zr));
	r = uvm_ioctl(fd, UVM_CLEAN_UP_ZOMBIE_RESOURCES, &zr);
	CHECK(r == 0 && zr.rmStatus == NV_OK, "CLEAN_UP_ZOMBIE_RESOURCES: NV_OK");
	/* the UUID table: entry 0 is the CPU, the rest zero without GPUs */
	memset(&ut, 0, sizeof(ut));
	memset(table, 0xee, sizeof(table));
	ut.tablePtr = (NvU64)(uintptr_t)table;
	r = uvm_ioctl(fd, UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_V2, &ut);
	CHECK(r == 0 && ut.rmStatus == NV_OK && memcmp(table[0].uuid, NV_PROCESSOR_UUID_CPU_DEFAULT.uuid, 16) == 0 &&
	    table[1].uuid[0] == 0 && table[256].uuid[15] == 0, "UUID table V2: CPU at 0, GPUs zero");
	ut.tablePtr = 0x10;
	r = uvm_ioctl(fd, UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_V2, &ut);
	CHECK(r == 0 && ut.rmStatus == NV_ERR_INVALID_ADDRESS, "UUID table to a bad pointer: INVALID_ADDRESS");

	/* free: wrong length INVALID_ADDRESS, then the exact range */
	memset(&fr, 0, sizeof(fr));
	fr.base = base;
	fr.length = 0x8000;
	r = uvm_ioctl(fd, UVM_FREE, &fr);
	CHECK(r == 0 && fr.rmStatus == NV_ERR_INVALID_ADDRESS, "FREE with the wrong length: INVALID_ADDRESS");
	fr.length = 0x10000;
	r = uvm_ioctl(fd, UVM_FREE, &fr);
	CHECK(r == 0 && fr.rmStatus == NV_OK, "FREE of the external range: NV_OK");
	r = uvm_ioctl(fd, UVM_VALIDATE_VA_RANGE, &vr);
	vr.length = 0x10000;
	r = uvm_ioctl(fd, UVM_VALIDATE_VA_RANGE, &vr);
	CHECK(r == 0 && vr.rmStatus == NV_ERR_INVALID_ADDRESS, "VALIDATE after FREE: INVALID_ADDRESS");
	close(fd);
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
