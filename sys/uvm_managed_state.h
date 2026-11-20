/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_managed_state.h - the decisions behind a managed range's mmap and
 * its lifetime (uvm-spec r1 01 section 4.1, 03 section 4.1), as pure
 * functions so that userland tests check every row.
 *
 * Lifetime.  A managed range is born at mmap together with a "managed
 * object" (the handle of its device pager object).  Two things end it:
 * the pager object dying (munmap of the last mapping, process exit) and
 * the VA space being destroyed (last close of the fd).  The two can come
 * in either order, and at process exit the fd closes first.  So the pair
 * is a two-state machine under one lock:
 *
 *   LINKED    the range is in a live VA space and the object points at it
 *   DETACHED  the VA space is gone (the range was torn down there);
 *             the object keeps its memory for the CPU view only
 *
 *   pager dtor in LINKED:    destroy the range (wait for the GPU, unmap,
 *                            leave the tree), free the object
 *   pager dtor in DETACHED:  free the object
 *   VA space destroy, LINKED:  detach (object forgets the range); the
 *                            destroy path tears the range down itself
 *   VA space destroy, DETACHED: nothing (no range is left to find)
 */

#ifndef UVM_MANAGED_STATE_H
#define UVM_MANAGED_STATE_H

#ifndef _KERNEL
#include <stdbool.h>
#include <stdint.h>
#endif

#include "nvidia_uvm_status.h"

enum uvm_mobj_state { UVM_MOBJ_LINKED = 1, UVM_MOBJ_DETACHED };
enum uvm_mobj_event { UVM_MOBJ_PAGER_DTOR = 1, UVM_MOBJ_VAS_DESTROY };

/* what the caller does about an event */
enum uvm_mobj_action {
	UVM_MOBJ_DO_NOTHING = 0,
	UVM_MOBJ_DO_DESTROY_RANGE_AND_FREE,	/* tear the range down, then free the object */
	UVM_MOBJ_DO_FREE,			/* free the object only */
	UVM_MOBJ_DO_DETACH			/* the object forgets the range */
};

/* the next state of an object after an event, and the action to take */
static inline enum uvm_mobj_action
uvm_mobj_event_decide(enum uvm_mobj_state state, enum uvm_mobj_event ev, enum uvm_mobj_state *next)
{
	*next = state;
	if (ev == UVM_MOBJ_PAGER_DTOR)
		return (state == UVM_MOBJ_LINKED ? UVM_MOBJ_DO_DESTROY_RANGE_AND_FREE : UVM_MOBJ_DO_FREE);
	if (state == UVM_MOBJ_LINKED) {
		*next = UVM_MOBJ_DETACHED;
		return (UVM_MOBJ_DO_DETACH);
	}
	return (UVM_MOBJ_DO_NOTHING);
}

/*
 * uvm_managed_mmap_check - the argument checks of a managed mmap that do
 * not need the VA space.  The offset is the address the UMD maps (rule 4);
 * the pager cannot see the virtual address itself, so the range starts at
 * the offset and the UMD's fixed mapping makes the two equal.  A page
 * aligned, non-empty, non-wrapping [offset, offset+size) is required
 * (INVALID_ADDRESS, which is EINVAL for the caller).
 */
static inline NV_STATUS
uvm_managed_mmap_check(uint64_t offset, uint64_t size, uint64_t page_size)
{
	if (size == 0 || (offset & (page_size - 1)) != 0 || (size & (page_size - 1)) != 0)
		return (NV_ERR_INVALID_ADDRESS);
	if (offset + size - 1 < offset)
		return (NV_ERR_INVALID_ADDRESS);
	return (NV_OK);
}

#endif /* UVM_MANAGED_STATE_H */
