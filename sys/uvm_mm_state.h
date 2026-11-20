/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_mm_state.h - the decision table of UVM_MM_INITIALIZE (uvm-spec r1
 * 01 section 3.4) and the bound memory map's states (03 section 2.1), as
 * pure functions so that userland tests check every row.
 */

#ifndef UVM_MM_STATE_H
#define UVM_MM_STATE_H

#ifndef _KERNEL
#include <stdbool.h>
#endif

#include "nvidia_uvm_status.h"

/* 03 section 2.1: bound to the creating process; a users reference held by an MM fd; shut down */
enum uvm_mm_state { UVM_MM_UNINITIALIZED = 0, UVM_MM_ALIVE, UVM_MM_RELEASED };

/* what the calling fd is (01 section 3.2) */
enum uvm_mm_caller { UVM_MM_CALLER_UNINIT, UVM_MM_CALLER_INITIALISING, UVM_MM_CALLER_VA_SPACE, UVM_MM_CALLER_MM };

/*
 * uvm_mm_initialize_decide - the status of UVM_MM_INITIALIZE, in the order
 * of the spec's list: the target checks come first (INVALID_ARGUMENT when
 * uvmFd is not one of our fds in the VA-space state), then the calling fd
 * (IN_USE when it is a VA-space fd or already an MM fd), then the VA
 * space's binding: no bound map on this platform configuration
 * (NOTHING_TO_DO), already ALIVE (INVALID_STATE), already RELEASED
 * (PAGE_TABLE_NOT_AVAIL).  NV_OK means: bind, and the fd becomes MM.
 * An fd that another thread is initialising answers BUSY_RETRY, as 3.2 does.
 */
static inline NV_STATUS
uvm_mm_initialize_decide(bool target_is_va_space_fd, enum uvm_mm_caller caller, bool vas_has_mm,
    enum uvm_mm_state state)
{
	if (!target_is_va_space_fd)
		return (NV_ERR_INVALID_ARGUMENT);
	if (caller == UVM_MM_CALLER_INITIALISING)
		return (NV_ERR_BUSY_RETRY);
	if (caller == UVM_MM_CALLER_VA_SPACE || caller == UVM_MM_CALLER_MM)
		return (NV_ERR_IN_USE);
	if (!vas_has_mm)
		return (NV_WARN_NOTHING_TO_DO);
	if (state == UVM_MM_ALIVE)
		return (NV_ERR_INVALID_STATE);
	if (state == UVM_MM_RELEASED)
		return (NV_ERR_PAGE_TABLE_NOT_AVAIL);
	return (NV_OK);
}

/*
 * uvm_mm_shutdown_needed - the "mm shutdown" sequence runs once: at the
 * release of the MM fd, or at VA space destroy when no MM fd was used.
 * Whichever comes second finds the state RELEASED and does nothing.
 */
static inline bool
uvm_mm_shutdown_needed(enum uvm_mm_state state)
{
	return (state != UVM_MM_RELEASED);
}

#endif /* UVM_MM_STATE_H */
