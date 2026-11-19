/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * core_range_test.c - the neutral interval tree of sys/core/uvm_core_range.h
 * behaves as the core/backend split of docs/ARCHITECTURE.md needs it to: a
 * range is found by the address it covers, a range that intersects another
 * one is found at both of its edges, an insertion that would overlap is
 * refused, a removed range is gone, the first fit lands where it can and
 * keeps the alignment it is given, and the tree walks in address order.
 *
 * The tree is the core's own: it holds no lock and allocates nothing, so
 * the test owns every range it puts in it.
 *
 * Red first: compiled before the header existed.
 */

#include <stdint.h>
#include <stdio.h>

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

/* #lizard forgives -- a list of independent checks, one branch each */
int
main(void)
{
	struct uvm_core_range_tree tree;
	struct uvm_core_range r1 = { 0 }, r2 = { 0 }, r3 = { 0 };
	struct uvm_core_range dup = { 0 };
	struct uvm_core_range *found;
	uint64_t out;

	/* ---- an empty tree ---- */
	uvm_core_range_tree_init(&tree);
	CHECK(uvm_core_range_first(&tree) == NULL, "an empty tree has no first range");
	CHECK(uvm_core_range_find(&tree, 0x1000) == NULL, "an empty tree finds nothing");
	CHECK(uvm_core_range_find_overlap(&tree, 0x1000, 0x1fff) == NULL,
	    "an empty tree finds no overlap");
	CHECK(uvm_core_range_find_free(&tree, 0x1000, 0x10000, 0x1000, 0x1000, &out) == 0 &&
	    out == 0x1000, "the first fit lands at the base when the tree is empty");

	/* ---- insert ---- */
	r1.start = 0x1000;
	r1.end = 0x1fff;
	r2.start = 0x4000;
	r2.end = 0x4fff;
	r3.start = 0x9000;
	r3.end = 0x9fff;
	CHECK(uvm_core_range_insert(&tree, &r1) == 0, "the first range is inserted");
	CHECK(uvm_core_range_insert(&tree, &r2) == 0, "the second range is inserted");
	CHECK(uvm_core_range_insert(&tree, &r3) == 0, "the third range is inserted");

	/* ---- find by address ---- */
	found = uvm_core_range_find(&tree, 0x1000);
	CHECK(found == &r1, "the first address of the first range is found");
	found = uvm_core_range_find(&tree, 0x1800);
	CHECK(found == &r1, "an address inside the first range is found");
	found = uvm_core_range_find(&tree, 0x1fff);
	CHECK(found == &r1, "the last address of the first range is found");
	found = uvm_core_range_find(&tree, 0x0fff);
	CHECK(found == NULL, "an address before the first range is not found");
	found = uvm_core_range_find(&tree, 0xa000);
	CHECK(found == NULL, "an address after the last range is not found");
	found = uvm_core_range_find(&tree, 0x2000);
	CHECK(found == NULL, "an address in the gap between two ranges is not found");

	/* ---- overlap, at both edges ---- */
	found = uvm_core_range_find_overlap(&tree, 0x0fff, 0x1000);
	CHECK(found == &r1, "an overlap at the low edge is found");
	found = uvm_core_range_find_overlap(&tree, 0x1fff, 0x2000);
	CHECK(found == &r1, "an overlap at the high edge is found");
	found = uvm_core_range_find_overlap(&tree, 0x4000, 0x4fff);
	CHECK(found == &r2, "a range that is exactly the second one is an overlap");
	found = uvm_core_range_find_overlap(&tree, 0x2000, 0x3fff);
	CHECK(found == NULL, "a range in the gap is not an overlap");
	found = uvm_core_range_find_overlap(&tree, 0xa000, 0xb000);
	CHECK(found == NULL, "a range after the last one is not an overlap");

	/* ---- an overlapping insert is refused ---- */
	dup.start = 0x1800;
	dup.end = 0x2800;
	CHECK(uvm_core_range_insert(&tree, &dup) == EEXIST,
	    "an insert that overlaps an existing range is refused");
	dup.start = 0x1000;
	dup.end = 0x1fff;
	CHECK(uvm_core_range_insert(&tree, &dup) == EEXIST,
	    "an insert of the same range again is refused");
	found = uvm_core_range_find(&tree, 0x2000);
	CHECK(found == NULL, "the refused insert left nothing behind");

	/* ---- the first fit ---- */
	CHECK(uvm_core_range_find_free(&tree, 0x1000, 0x10000, 0x1000, 0x1000, &out) == 0 &&
	    out == 0x2000, "the first fit lands after the range that is in the way");
	CHECK(uvm_core_range_find_free(&tree, 0x1001, 0x10000, 0x1000, 0x1000, &out) == 0 &&
	    out == 0x2000, "the first fit keeps the alignment it is given");
	CHECK(uvm_core_range_find_free(&tree, 0x1000, 0x1000, 0x1000, 0x1000, &out) == ENOSPC,
	    "a window that is entirely taken does not fit");
	CHECK(uvm_core_range_find_free(&tree, UINT64_MAX - 0x100, 0x200, 0x100, 0x100, &out) == ENOSPC,
	    "a window whose length overflows does not fit");
	CHECK(uvm_core_range_find_free(&tree, UINT64_MAX - 0xFFE, 0xFF0, 0x100, 0x1000, &out) == ENOSPC,
	    "a base whose alignment rounds past the end of the address space does not fit below it");

	/* ---- remove ---- */
	uvm_core_range_remove(&tree, &r1);
	found = uvm_core_range_find(&tree, 0x1000);
	CHECK(found == NULL, "a removed range is not found by its address");
	found = uvm_core_range_find_overlap(&tree, 0x1000, 0x1fff);
	CHECK(found == NULL, "a removed range is not an overlap either");
	found = uvm_core_range_find(&tree, 0x4000);
	CHECK(found == &r2, "the other ranges are still there");

	/* ---- the order of the walk ---- */
	found = uvm_core_range_first(&tree);
	CHECK(found == &r2, "the first range of the walk is the lowest one");
	found = uvm_core_range_next(&tree, found);
	CHECK(found == &r3, "the walk goes to the next range in address order");
	found = uvm_core_range_next(&tree, found);
	CHECK(found == NULL, "the walk ends after the last range");

	if (fails != 0) {
		printf("%d check(s) failed\n", fails);
		return (1);
	}
	printf("core_range_test: ok\n");
	return (0);
}
