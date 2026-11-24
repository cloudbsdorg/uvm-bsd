/*
 * Copyright (c) 2026 SkunkWerks, GmbH
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * fb_state_test.c - the rules for a GPU without video memory
 * (sys/uvm_fb_state.h): when the frame buffer description means "no video
 * memory", when a missing PMA object is normal, and where the GPFIFO goes.
 * The two descriptions used are the ones RM gives: all zero for a zero-FB
 * GPU (GB10), a heap and an allocatable range for a discrete GPU.
 */

#include <stdio.h>
#include <string.h>

#include "uvm_fb_state.h"

static int fails;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

int
main(void)
{
	UvmGpuFbInfo zero, discrete, broken, part;

	/* RM clears the whole description on a zero-FB GPU, bZeroFb included */
	memset(&zero, 0, sizeof(zero));
	/* a 16 GB discrete GPU: sizes in KB */
	memset(&discrete, 0, sizeof(discrete));
	discrete.heapSize = 16u * 1024 * 1024;
	discrete.reservedHeapSize = 64 * 1024;
	discrete.maxAllocatableAddress = (16ull << 30) - 1;
	discrete.maxVidmemPageSize = 2u << 20;
	/* a GPU whose frame buffer RM reports as broken, sizes still filled */
	broken = discrete;
	broken.bZeroFb = 1;

	CHECK(uvm_fb_no_video_memory(&zero), "an all-zero description has no video memory");
	CHECK(!uvm_fb_no_video_memory(&discrete), "a discrete GPU has video memory");
	CHECK(uvm_fb_no_video_memory(&broken), "a broken frame buffer is no video memory");

	/* any one sign of a frame buffer is enough to keep the old behaviour */
	part = zero;
	part.heapSize = 1;
	CHECK(!uvm_fb_no_video_memory(&part), "a heap size means video memory");
	part = zero;
	part.reservedHeapSize = 1;
	CHECK(!uvm_fb_no_video_memory(&part), "a reserved heap size means video memory");
	part = zero;
	part.maxAllocatableAddress = 4095;
	CHECK(!uvm_fb_no_video_memory(&part), "an allocatable address means video memory");
	/* fields that say nothing about the heap do not count */
	part = zero;
	part.maxVidmemPageSize = 2u << 20;
	part.staticBar1Size = 1;
	CHECK(uvm_fb_no_video_memory(&part), "page size and BAR1 fields alone are not video memory");

	/* the PMA object: absent is normal only for that status on such a GPU */
	CHECK(uvm_fb_pma_absent_ok(NV_ERR_OBJECT_NOT_FOUND, &zero), "no PMA on a zero-FB GPU is normal");
	CHECK(uvm_fb_pma_absent_ok(NV_ERR_OBJECT_NOT_FOUND, &broken), "no PMA on a broken FB is normal");
	CHECK(!uvm_fb_pma_absent_ok(NV_ERR_OBJECT_NOT_FOUND, &discrete), "no PMA on a discrete GPU is an error");
	CHECK(!uvm_fb_pma_absent_ok(NV_OK, &zero), "success is not an absent PMA");
	CHECK(!uvm_fb_pma_absent_ok(NV_OK, &discrete), "success is not an absent PMA (discrete)");
	CHECK(!uvm_fb_pma_absent_ok(NV_ERR_NOT_SUPPORTED, &zero), "another status stays an error");
	CHECK(!uvm_fb_pma_absent_ok(NV_ERR_INVALID_ARGUMENT, &zero), "another status stays an error");
	CHECK(!uvm_fb_pma_absent_ok(NV_ERR_NO_MEMORY, &zero), "another status stays an error");
	CHECK(!uvm_fb_pma_absent_ok(NV_ERR_GENERIC, &discrete), "another status stays an error (discrete)");

	/* the GPFIFO location: the tunable decides only where video memory exists */
	CHECK(uvm_fb_gpfifo_in_sysmem(true, &discrete), "tunable 1 on a discrete GPU: system memory");
	CHECK(!uvm_fb_gpfifo_in_sysmem(false, &discrete), "tunable 0 on a discrete GPU: video memory");
	CHECK(uvm_fb_gpfifo_in_sysmem(true, &zero), "tunable 1 on a zero-FB GPU: system memory");
	CHECK(uvm_fb_gpfifo_in_sysmem(false, &zero), "tunable 0 on a zero-FB GPU: still system memory");

	if (fails == 0)
		printf("fb_state_test: all rows pass\n");
	return (fails != 0);
}
