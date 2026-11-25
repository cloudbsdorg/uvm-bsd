/*
 * Copyright (c) 2026 SkunkWerks, GmbH
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_fb_state.h - what follows from a GPU that has no video memory, as
 * pure functions so that userland tests check every row.
 *
 * An integrated GPU (the GB10 SoC, chip GB20B) has no frame buffer: all of
 * its memory is system memory.  RM marks such a GPU "zero FB" and then
 *   - returns the frame buffer description zeroed: it skips the queries and
 *     clears the whole structure, so the bZeroFb field reads false there
 *     too (that field is RM's "FB is broken" answer, which is only asked on
 *     a GPU that has a frame buffer);
 *   - has no physical memory allocator (PMA) for video memory, because the
 *     PMA is set up only when frame buffer regions exist, and answers
 *     NV_ERR_OBJECT_NOT_FOUND to the request for the PMA object;
 *   - cannot give a buffer in video memory.
 * The rule here: a GPU has no video memory when RM says the frame buffer is
 * absent or broken, or when the description shows no heap and no
 * allocatable address at all.
 */

#ifndef UVM_FB_STATE_H
#define UVM_FB_STATE_H

#ifndef _KERNEL
#include <stdbool.h>
#endif

#include "nvidia_uvm_rm.h"

/* true when the GPU has no video memory to allocate from */
static inline bool
uvm_fb_no_video_memory(const UvmGpuFbInfo *fb)
{
	if (fb->bZeroFb)
		return (true);
	return (fb->heapSize == 0 && fb->reservedHeapSize == 0 && fb->maxAllocatableAddress == 0);
}

/*
 * uvm_fb_pma_absent_ok - true when the answer RM gave to the request for
 * the PMA object means "this GPU has none and needs none": exactly
 * NV_ERR_OBJECT_NOT_FOUND on a GPU without video memory.  Any other
 * failure, and that failure on a GPU with video memory, stays an error.
 */
static inline bool
uvm_fb_pma_absent_ok(NV_STATUS status, const UvmGpuFbInfo *fb)
{
	return (status == NV_ERR_OBJECT_NOT_FOUND && uvm_fb_no_video_memory(fb));
}

/*
 * uvm_fb_gpfifo_in_sysmem - where the GPFIFO ring and the PUT word go: the
 * tunable decides on a GPU with video memory; without video memory system
 * memory is the only place.
 */
static inline bool
uvm_fb_gpfifo_in_sysmem(bool tunable_sysmem, const UvmGpuFbInfo *fb)
{
	return (tunable_sysmem || uvm_fb_no_video_memory(fb));
}

#endif /* UVM_FB_STATE_H */
