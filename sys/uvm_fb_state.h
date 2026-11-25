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
 *
 * "This GPU has no video memory" and "the frame buffer information is
 * broken or missing" are two different things and must never be
 * conflated: only the first makes a missing PMA object normal, and only
 * the first makes the GPFIFO placement a rule instead of an accident.
 *
 *   UVM_FB_USABLE  the GPU has video memory and RM describes it: a heap, a
 *                  reserved heap or an allocatable address is present and
 *                  RM does not call the frame buffer broken;
 *   UVM_FB_ABSENT  RM cleared the whole description: this GPU has no video
 *                  memory at all (the integrated GB10);
 *   UVM_FB_BROKEN  the information is broken or missing: the request for
 *                  the description failed, or RM calls the frame buffer
 *                  broken (bZeroFb) while describing a heap, or the
 *                  description is empty although the RM hands out a PMA
 *                  object (a PMA exists only where there is video memory).
 *
 * The rules live here as pure functions, so that the userland unit test
 * checks every row of them and the kernel code only applies them.
 */

#ifndef UVM_FB_STATE_H
#define UVM_FB_STATE_H

#ifndef _KERNEL
#include <stdbool.h>
#endif

#include "nvidia_uvm_rm.h"

/* the state of the frame buffer information of a GPU */
enum uvm_fb_state {
	UVM_FB_USABLE = 0,	/* the GPU has video memory and RM describes it */
	UVM_FB_ABSENT,		/* the GPU has no video memory: RM cleared the description */
	UVM_FB_BROKEN		/* the frame buffer information is broken or missing */
};

/* what the registration does with the answer to the request for the PMA object */
enum uvm_fb_pma_decision {
	UVM_FB_PMA_USE = 0,	/* the object is there: register the eviction callbacks */
	UVM_FB_PMA_ABSENT,	/* no video memory, so no PMA object and none needed */
	UVM_FB_PMA_FAIL		/* the information does not add up: fail the registration */
};

/* where the GPFIFO ring and the PUT word go */
enum uvm_fb_gpfifo_place {
	UVM_FB_GPFIFO_VIDEO = 0,		/* video memory */
	UVM_FB_GPFIFO_SYSMEM_TUNABLE,	/* system memory: the tunable asked for it */
	UVM_FB_GPFIFO_SYSMEM_NO_VIDEO,	/* system memory: the GPU has no video memory */
	UVM_FB_GPFIFO_SYSMEM_UNUSABLE	/* system memory: the frame buffer information is broken */
};

/* the verdict on the PMA object, with the reason for it */
struct uvm_fb_pma_verdict {
	enum uvm_fb_pma_decision	decision;
	const char			*reason;
};

/*
 * uvm_fb_info_state - the state of the frame buffer information.
 *
 * fb_status is what the RM answered to the request for the description: an
 * error there means the description is missing, not that the GPU has no
 * video memory.  bZeroFb is RM's "the frame buffer is broken" answer, and
 * it is asked only on a GPU that has a frame buffer, so it never means
 * "no video memory".
 */
static inline enum uvm_fb_state
uvm_fb_info_state(NV_STATUS fb_status, const UvmGpuFbInfo *fb)
{
	if (fb_status != NV_OK)
		return (UVM_FB_BROKEN);
	if (fb->bZeroFb)
		return (UVM_FB_BROKEN);
	if (fb->heapSize == 0 && fb->reservedHeapSize == 0 && fb->maxAllocatableAddress == 0)
		return (UVM_FB_ABSENT);
	return (UVM_FB_USABLE);
}

/* true only for a GPU that really has no video memory */
static inline bool
uvm_fb_no_video_memory(const UvmGpuFbInfo *fb)
{
	return (uvm_fb_info_state(NV_OK, fb) == UVM_FB_ABSENT);
}

/*
 * uvm_fb_pma_judge - what to do with the answer to the request for the PMA
 * object, and why.
 *
 * A missing PMA object is normal only on a GPU that has no video memory:
 * the PMA is set up only where frame buffer regions exist.  A PMA object on
 * a GPU whose description says it has no video memory is a contradiction,
 * and the contradiction is in the description, not in the video memory.
 */
static inline struct uvm_fb_pma_verdict
uvm_fb_pma_judge(enum uvm_fb_state state, NV_STATUS status)
{
	struct uvm_fb_pma_verdict v;

	switch (state) {
	case UVM_FB_ABSENT:
		if (status == NV_ERR_OBJECT_NOT_FOUND) {
			v.decision = UVM_FB_PMA_ABSENT;
			v.reason = "the GPU has no video memory and therefore no PMA object";
			return (v);
		}
		v.decision = UVM_FB_PMA_FAIL;
		if (status == NV_OK)
			v.reason = "the RM gave a PMA object although the description says "
			    "the GPU has no video memory";
		else
			v.reason = "the request for the PMA object failed on a GPU without video memory";
		return (v);
	case UVM_FB_BROKEN:
		v.decision = UVM_FB_PMA_FAIL;
		v.reason = "the frame buffer information is broken or missing";
		return (v);
	case UVM_FB_USABLE:
	default:
		break;
	}
	if (status == NV_OK) {
		v.decision = UVM_FB_PMA_USE;
		v.reason = "the GPU has video memory and gave a PMA object";
		return (v);
	}
	v.decision = UVM_FB_PMA_FAIL;
	if (status == NV_ERR_OBJECT_NOT_FOUND)
		v.reason = "the GPU has video memory but gave no PMA object";
	else
		v.reason = "the request for the PMA object failed";
	return (v);
}

/*
 * uvm_fb_gpfifo_place - where the GPFIFO ring and the PUT word go, and why.
 *
 * The tunable decides only on a GPU that has video memory; without video
 * memory system memory is the only place.  The reason travels with the
 * place, so that the message the driver prints and the placement it makes
 * cannot disagree.
 */
static inline enum uvm_fb_gpfifo_place
uvm_fb_gpfifo_place(bool tunable_sysmem, enum uvm_fb_state state)
{
	switch (state) {
	case UVM_FB_ABSENT:
		return (UVM_FB_GPFIFO_SYSMEM_NO_VIDEO);
	case UVM_FB_BROKEN:
		return (UVM_FB_GPFIFO_SYSMEM_UNUSABLE);
	case UVM_FB_USABLE:
	default:
		break;
	}
	return (tunable_sysmem ? UVM_FB_GPFIFO_SYSMEM_TUNABLE : UVM_FB_GPFIFO_VIDEO);
}

/* true when the GPFIFO goes to system memory */
static inline bool
uvm_fb_gpfifo_is_sysmem(enum uvm_fb_gpfifo_place place)
{
	return (place != UVM_FB_GPFIFO_VIDEO);
}

/* the RM buffer location of the GPFIFO ring and of the PUT word */
static inline NvU32
uvm_fb_gpfifo_location(bool sysmem)
{
	return (sysmem ? UVM_BUFFER_LOCATION_SYS : UVM_BUFFER_LOCATION_VID);
}

/* the name of a state, for the log */
static inline const char *
uvm_fb_state_name(enum uvm_fb_state state)
{
	switch (state) {
	case UVM_FB_ABSENT:
		return ("absent");
	case UVM_FB_BROKEN:
		return ("broken");
	case UVM_FB_USABLE:
	default:
		break;
	}
	return ("usable");
}

#endif /* UVM_FB_STATE_H */
