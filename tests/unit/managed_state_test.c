/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * managed_state_test.c - the managed mmap argument rules and the
 * lifetime state machine of a managed range and its pager object
 * (sys/uvm_managed_state.h), in both orders of death, and the
 * collision and coverage rules of the range tree they sit on.
 */

#include <stdio.h>

#include "uvm_managed_state.h"
#include "core/uvm_core_range.h"

static int fails;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

static bool
is_odd_start(const struct uvm_core_range *r)
{
	return ((r->start & 0x1000) != 0);
}

int
main(void)
{
	enum uvm_mobj_state st;
	struct uvm_core_range_tree tree;
	struct uvm_core_range a = { .start = 0x1000, .end = 0x1fff }, b = { .start = 0x2000, .end = 0x3fff },
	    c = { .start = 0x5000, .end = 0x5fff }, clash = { .start = 0x1800, .end = 0x27ff };

	/* mmap arguments */
	CHECK(uvm_managed_mmap_check(0x5fe00000, 0x400000, 4096) == NV_OK, "libcuda's 4 MiB mapping is accepted");
	CHECK(uvm_managed_mmap_check(0x5fe00001, 0x400000, 4096) == NV_ERR_INVALID_ADDRESS, "unaligned offset");
	CHECK(uvm_managed_mmap_check(0x5fe00000, 0, 4096) == NV_ERR_INVALID_ADDRESS, "empty");
	CHECK(uvm_managed_mmap_check(0x5fe00000, 0x400100, 4096) == NV_ERR_INVALID_ADDRESS, "unaligned size");
	CHECK(uvm_managed_mmap_check(UINT64_MAX - 0xfff, 0x2000, 4096) == NV_ERR_INVALID_ADDRESS, "wraps");
	CHECK(uvm_managed_mmap_check(UINT64_MAX - 0xfff, 0x1000, 4096) == NV_OK, "last page is fine");

	/* lifetime: pager dies first (munmap) */
	CHECK(uvm_mobj_event_decide(UVM_MOBJ_LINKED, UVM_MOBJ_PAGER_DTOR, &st) == UVM_MOBJ_DO_DESTROY_RANGE_AND_FREE,
	    "munmap with a live VA space destroys the range");
	/* VA space dies first (process exit: the fd closes before the map is torn down) */
	CHECK(uvm_mobj_event_decide(UVM_MOBJ_LINKED, UVM_MOBJ_VAS_DESTROY, &st) == UVM_MOBJ_DO_DETACH &&
	    st == UVM_MOBJ_DETACHED, "VA space destroy detaches the object");
	CHECK(uvm_mobj_event_decide(UVM_MOBJ_DETACHED, UVM_MOBJ_PAGER_DTOR, &st) == UVM_MOBJ_DO_FREE,
	    "a later pager dtor only frees the object");
	CHECK(uvm_mobj_event_decide(UVM_MOBJ_DETACHED, UVM_MOBJ_VAS_DESTROY, &st) == UVM_MOBJ_DO_NOTHING &&
	    st == UVM_MOBJ_DETACHED, "nothing is left for a second VA space event");
	CHECK(uvm_mobj_event_decide(UVM_MOBJ_LINKED, UVM_MOBJ_PAGER_DTOR, &st) == UVM_MOBJ_DO_DESTROY_RANGE_AND_FREE &&
	    st == UVM_MOBJ_LINKED, "dtor leaves the state to the caller (the object is freed)");

	/* range lookup, collision and coverage */
	uvm_core_range_tree_init(&tree);
	CHECK(uvm_core_range_insert(&tree, &a) == 0 && uvm_core_range_insert(&tree, &b) == 0 &&
	    uvm_core_range_insert(&tree, &c) == 0, "three ranges insert");
	CHECK(uvm_core_range_insert(&tree, &clash) != 0, "an overlapping mmap collides (ADDRESS_IN_USE)");
	CHECK(uvm_core_range_find(&tree, 0x2abc) == &b, "lookup by address");
	CHECK(uvm_core_range_covered(&tree, 0x1000, 0x3fff, NULL), "adjacent ranges cover the interval");
	CHECK(uvm_core_range_covered(&tree, 0x1800, 0x2fff, NULL), "a sub-interval across two ranges is covered");
	CHECK(!uvm_core_range_covered(&tree, 0x1000, 0x5fff, NULL), "a gap (0x4000) is not covered");
	CHECK(!uvm_core_range_covered(&tree, 0x0, 0x1fff, NULL), "a start before the first range is not covered");
	CHECK(!uvm_core_range_covered(&tree, 0x5000, 0x6000, NULL), "an end past the last range is not covered");
	CHECK(!uvm_core_range_covered(&tree, 0x1000, 0x3fff, is_odd_start), "a rejected range type breaks coverage");
	CHECK(uvm_core_range_covered(&tree, 0x1000, 0x1fff, is_odd_start), "the one accepted range covers itself");
	CHECK(uvm_core_range_covered(&tree, UINT64_MAX - 1, UINT64_MAX, NULL) == false, "no range at the very end");
	uvm_core_range_remove(&tree, &b);
	CHECK(!uvm_core_range_covered(&tree, 0x1000, 0x3fff, NULL), "a removed range leaves a gap");

	if (fails != 0) {
		printf("%d check(s) failed\n", fails);
		return (1);
	}
	printf("managed_state_test: ok\n");
	return (0);
}
