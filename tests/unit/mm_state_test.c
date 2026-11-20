/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * mm_state_test.c - every row of the UVM_MM_INITIALIZE status list of
 * uvm-spec r1 01 section 3.4, and the once-only mm shutdown of 03 section
 * 2.1, against uvm_mm_state.h.
 */

#include <stdio.h>

#include "uvm_mm_state.h"

static int fails;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

#define	DECIDE(t, c, mm, st)	uvm_mm_initialize_decide((t), (c), (mm), (st))

int
main(void)
{
	/* uvmFd is not a UVM fd, or not in the VA-space state: first, whatever else holds */
	CHECK(DECIDE(false, UVM_MM_CALLER_UNINIT, true, UVM_MM_UNINITIALIZED) == NV_ERR_INVALID_ARGUMENT,
	    "bad target: INVALID_ARGUMENT");
	CHECK(DECIDE(false, UVM_MM_CALLER_VA_SPACE, true, UVM_MM_ALIVE) == NV_ERR_INVALID_ARGUMENT,
	    "bad target beats IN_USE and INVALID_STATE");
	/* the calling fd is itself a VA-space fd or already an MM fd: IN_USE, after the target checks */
	CHECK(DECIDE(true, UVM_MM_CALLER_VA_SPACE, true, UVM_MM_UNINITIALIZED) == NV_ERR_IN_USE,
	    "caller is a VA-space fd: IN_USE");
	CHECK(DECIDE(true, UVM_MM_CALLER_MM, true, UVM_MM_ALIVE) == NV_ERR_IN_USE,
	    "caller is an MM fd: IN_USE");
	CHECK(DECIDE(true, UVM_MM_CALLER_INITIALISING, true, UVM_MM_UNINITIALIZED) == NV_ERR_BUSY_RETRY,
	    "caller being initialised: BUSY_RETRY");
	/* no bound map in this configuration: the platform needs no secondary fd */
	CHECK(DECIDE(true, UVM_MM_CALLER_UNINIT, false, UVM_MM_UNINITIALIZED) == NV_WARN_NOTHING_TO_DO,
	    "no bound map: NOTHING_TO_DO");
	/* the binding states */
	CHECK(DECIDE(true, UVM_MM_CALLER_UNINIT, true, UVM_MM_ALIVE) == NV_ERR_INVALID_STATE,
	    "binding already ALIVE: INVALID_STATE");
	CHECK(DECIDE(true, UVM_MM_CALLER_UNINIT, true, UVM_MM_RELEASED) == NV_ERR_PAGE_TABLE_NOT_AVAIL,
	    "binding RELEASED: PAGE_TABLE_NOT_AVAIL");
	CHECK(DECIDE(true, UVM_MM_CALLER_UNINIT, true, UVM_MM_UNINITIALIZED) == NV_OK,
	    "the good case: NV_OK, the fd becomes MM");

	/* the shutdown runs once, from whichever side comes first */
	CHECK(uvm_mm_shutdown_needed(UVM_MM_UNINITIALIZED), "no MM fd: destroy runs the shutdown");
	CHECK(uvm_mm_shutdown_needed(UVM_MM_ALIVE), "MM fd release runs the shutdown");
	CHECK(!uvm_mm_shutdown_needed(UVM_MM_RELEASED), "after the shutdown: nothing to do");

	if (fails == 0)
		printf("mm_state_test: all passed\n");
	return (fails != 0);
}
