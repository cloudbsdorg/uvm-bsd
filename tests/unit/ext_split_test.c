/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ext_split_test.c - the arithmetic that splits an external mapping around
 * an unmapped interval, of sys/uvm_ext_split.h, as uvm-spec r1 requires
 * of UVM_UNMAP_EXTERNAL and of the replacement of overlapping mappings at
 * map time (01 section 5.5): a mapping that only partly overlaps the
 * interval is split so that only the overlap is removed (03 section 4.2).
 *
 * The intervals are inclusive of both ends, as the mapping's [start, end]
 * is in sys/uvm.h.
 *
 * Red first: compiled before the header existed.
 */

#include <stdio.h>

#include "uvm_ext_split.h"

static int fails;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

/* #lizard forgives -- a list of independent checks, one branch each; splitting it would hide the spec table it mirrors */
int
main(void)
{
	struct uvm_ext_split_plan p;

	/* ---- no overlap: the mapping is untouched ---- */
	uvm_ext_split_plan(0x1000, 0x5000, 0x0000, 0x0fff, &p);
	CHECK(p.overlaps == 0, "unmap before: overlaps %d", p.overlaps);
	CHECK(p.keep_left == 0 && p.keep_right == 0, "unmap before: a part is kept");

	uvm_ext_split_plan(0x1000, 0x5000, 0x5001, 0x9000, &p);
	CHECK(p.overlaps == 0, "unmap after: overlaps %d", p.overlaps);
	CHECK(p.keep_left == 0 && p.keep_right == 0, "unmap after: a part is kept");

	/* adjacent: the intervals touch but do not intersect */
	uvm_ext_split_plan(0x1000, 0x5000, 0x0000, 0x0fff, &p);
	CHECK(p.overlaps == 0, "adjacent before: overlaps %d", p.overlaps);
	uvm_ext_split_plan(0x1000, 0x5000, 0x5001, 0x5001, &p);
	CHECK(p.overlaps == 0, "adjacent after: overlaps %d", p.overlaps);
	CHECK(p.keep_left == 0 && p.keep_right == 0, "adjacent: a part is kept");

	/* ---- the interval covers the mapping exactly: nothing is kept ---- */
	uvm_ext_split_plan(0x1000, 0x5000, 0x1000, 0x5000, &p);
	CHECK(p.overlaps == 1, "exact cover: overlaps %d", p.overlaps);
	CHECK(p.keep_left == 0 && p.keep_right == 0, "exact cover: a part is kept");

	/* ---- the interval is inside the mapping: both remainders stay ---- */
	uvm_ext_split_plan(0x1000, 0x5000, 0x2000, 0x3000, &p);
	CHECK(p.overlaps == 1, "inside: overlaps %d", p.overlaps);
	CHECK(p.keep_left == 1 && p.keep_right == 1, "inside: both parts are kept");
	CHECK(p.left_start == 0x1000 && p.left_end == 0x1fff, "inside: left [%#llx, %#llx]",
	    (unsigned long long)p.left_start, (unsigned long long)p.left_end);
	CHECK(p.right_start == 0x3001 && p.right_end == 0x5000, "inside: right [%#llx, %#llx]",
	    (unsigned long long)p.right_start, (unsigned long long)p.right_end);

	/* ---- the interval covers the start of the mapping only ---- */
	uvm_ext_split_plan(0x1000, 0x5000, 0x0000, 0x2000, &p);
	CHECK(p.overlaps == 1, "start: overlaps %d", p.overlaps);
	CHECK(p.keep_left == 0, "start: the left part is kept");
	CHECK(p.keep_right == 1, "start: the right part is not kept");
	CHECK(p.right_start == 0x2001 && p.right_end == 0x5000, "start: right [%#llx, %#llx]",
	    (unsigned long long)p.right_start, (unsigned long long)p.right_end);

	/* ---- the interval covers the end of the mapping only ---- */
	uvm_ext_split_plan(0x1000, 0x5000, 0x4000, 0x9000, &p);
	CHECK(p.overlaps == 1, "end: overlaps %d", p.overlaps);
	CHECK(p.keep_left == 1, "end: the left part is not kept");
	CHECK(p.keep_right == 0, "end: the right part is kept");
	CHECK(p.left_start == 0x1000 && p.left_end == 0x3fff, "end: left [%#llx, %#llx]",
	    (unsigned long long)p.left_start, (unsigned long long)p.left_end);

	/* ---- the interval covers the mapping and more: nothing is kept ---- */
	uvm_ext_split_plan(0x1000, 0x5000, 0x0000, 0x9000, &p);
	CHECK(p.overlaps == 1, "cover and more: overlaps %d", p.overlaps);
	CHECK(p.keep_left == 0 && p.keep_right == 0, "cover and more: a part is kept");

	/* ---- a one page mapping ---- */
	uvm_ext_split_plan(0x4000, 0x4fff, 0x4000, 0x4fff, &p);
	CHECK(p.overlaps == 1 && p.keep_left == 0 && p.keep_right == 0, "one page covered exactly");
	uvm_ext_split_plan(0x4000, 0x4fff, 0x5000, 0x5fff, &p);
	CHECK(p.overlaps == 0, "one page, unmap after: overlaps %d", p.overlaps);
	uvm_ext_split_plan(0x4000, 0x4fff, 0x3000, 0x3fff, &p);
	CHECK(p.overlaps == 0, "one page, unmap before: overlaps %d", p.overlaps);

	/* ---- the interval starts at the mapping's first page: the left part is gone ---- */
	uvm_ext_split_plan(0x1000, 0x5000, 0x1000, 0x1000, &p);
	CHECK(p.overlaps == 1 && p.keep_left == 0, "unmap the first page: the left part is kept");
	CHECK(p.keep_right == 1 && p.right_start == 0x1001 && p.right_end == 0x5000,
	    "unmap the first page: right [%#llx, %#llx]", (unsigned long long)p.right_start,
	    (unsigned long long)p.right_end);

	/* ---- the interval ends at the mapping's last page: the right part is gone ---- */
	uvm_ext_split_plan(0x1000, 0x5000, 0x5000, 0x5000, &p);
	CHECK(p.overlaps == 1 && p.keep_right == 0, "unmap the last page: the right part is kept");
	CHECK(p.keep_left == 1 && p.left_start == 0x1000 && p.left_end == 0x4fff,
	    "unmap the last page: left [%#llx, %#llx]", (unsigned long long)p.left_start,
	    (unsigned long long)p.left_end);

	if (fails == 0)
		printf("ext_split_test: PASS\n");
	return (fails != 0);
}
