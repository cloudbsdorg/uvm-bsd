/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_core_range.c - the neutral interval tree of the UVM core.
 *
 * The tree is a red-black tree over the first address of every range.  It
 * is bookkeeping: it holds no lock, it allocates nothing and it names no
 * vendor.  The caller owns the memory of the ranges and the locking of the
 * tree, so the core can be compiled and used outside the kernel.
 *
 * Every operation here is O(log n), except the first fit, which walks the
 * ranges that are in the way of the address it is looking for.
 */

#include "uvm_core_range.h"

/*
 * The order of the tree: by the first address of the range.  Two ranges
 * with the same first address cannot both be in the tree; the insertion
 * refuses the second one.
 * Time: O(1).  Space: O(1).
 */
static int
uvm_core_range_cmp(const struct uvm_core_range *a, const struct uvm_core_range *b)
{
	if (a->start < b->start)
		return (-1);
	return (a->start > b->start);
}

/* the tree's own operations stay in this file */
RB_PROTOTYPE_STATIC(uvm_core_range_tree, uvm_core_range, node, uvm_core_range_cmp);
RB_GENERATE_STATIC(uvm_core_range_tree, uvm_core_range, node, uvm_core_range_cmp);

/*
 * uvm_core_range_roundup - `value` rounded up to a multiple of `align`,
 * which is a power of two.  A value that cannot be rounded up without
 * wrapping around the end of the address space is reported as 0, and the
 * caller stops there.
 * Time: O(1).  Space: O(1).
 */
static uint64_t
uvm_core_range_roundup(uint64_t value, uint64_t align)
{
	uint64_t mask = align - 1;

	if (value > UINT64_MAX - mask)
		return (0);
	return ((value + mask) & ~mask);
}

/*
 * uvm_core_range_tree_init - an empty tree.
 * Time: O(1).  Space: O(1).
 */
void
uvm_core_range_tree_init(struct uvm_core_range_tree *tree)
{
	RB_INIT(tree);
}

/*
 * uvm_core_range_find - the range that covers `addr`, or NULL when there is
 * none.  The tree is ordered by the first address, so the range that can
 * cover `addr` is the one at or below it.
 * Time: O(log n).  Space: O(1).
 */
struct uvm_core_range *
uvm_core_range_find(struct uvm_core_range_tree *tree, uint64_t addr)
{
	struct uvm_core_range key = { 0 }, *r;

	key.start = addr;
	r = RB_NFIND(uvm_core_range_tree, tree, &key);
	if (r != NULL && r->start == addr)
		return (r);
	r = (r != NULL) ? RB_PREV(uvm_core_range_tree, tree, r) : RB_MAX(uvm_core_range_tree, tree);
	if (r != NULL && r->end >= addr)
		return (r);
	return (NULL);
}

/*
 * uvm_core_range_find_overlap - the first range that intersects
 * [start, end], or NULL when there is none.  The range that covers `start`
 * is the first one to be looked at; when there is none, the first range
 * that begins at or after `start` is the only one that can still reach
 * into the interval.
 * Time: O(log n).  Space: O(1).
 */
struct uvm_core_range *
uvm_core_range_find_overlap(struct uvm_core_range_tree *tree, uint64_t start, uint64_t end)
{
	struct uvm_core_range key = { 0 }, *r;

	r = uvm_core_range_find(tree, start);
	if (r != NULL)
		return (r);
	key.start = start;
	r = RB_NFIND(uvm_core_range_tree, tree, &key);
	if (r != NULL && r->start <= end)
		return (r);
	return (NULL);
}

/*
 * uvm_core_range_insert - put `range` in the tree.  EEXIST when it would
 * overlap a range that is already there, 0 otherwise.  The tree keeps the
 * range itself; the caller keeps its memory.
 * Time: O(log n).  Space: O(1).
 */
int
uvm_core_range_insert(struct uvm_core_range_tree *tree, struct uvm_core_range *range)
{
	if (uvm_core_range_find_overlap(tree, range->start, range->end) != NULL)
		return (EEXIST);
	RB_INSERT(uvm_core_range_tree, tree, range);
	return (0);
}

/*
 * uvm_core_range_remove - take `range` out of the tree.  The range itself
 * is left alone; the caller owns its memory.
 * Time: O(log n).  Space: O(1).
 */
void
uvm_core_range_remove(struct uvm_core_range_tree *tree, struct uvm_core_range *range)
{
	RB_REMOVE(uvm_core_range_tree, tree, range);
}

/*
 * uvm_core_range_first - the lowest range of the tree, or NULL when the
 * tree is empty.
 * Time: O(log n).  Space: O(1).
 */
struct uvm_core_range *
uvm_core_range_first(struct uvm_core_range_tree *tree)
{
	return (RB_MIN(uvm_core_range_tree, tree));
}

/*
 * uvm_core_range_next - the range that follows `range` in address order, or
 * NULL when `range` is the last one.
 * Time: O(log n).  Space: O(1).
 */
struct uvm_core_range *
uvm_core_range_next(struct uvm_core_range_tree *tree, struct uvm_core_range *range)
{
	/* the walk follows the parent links of the range itself */
	(void)tree;
	return (RB_NEXT(uvm_core_range_tree, tree, range));
}

/*
 * uvm_core_range_covered - [start, end] is covered by accepted ranges with
 * no gap.  Walks from the range that holds `start`: each range must begin
 * at or before the next uncovered address; the cursor then moves to the
 * address after the range's end (stopping at the top of the address space).
 * Time: O(log n + k).  Space: O(1).
 */
bool
uvm_core_range_covered(struct uvm_core_range_tree *tree, uint64_t start, uint64_t end,
    bool (*accept)(const struct uvm_core_range *))
{
	struct uvm_core_range *r;
	uint64_t cursor = start;

	if (end < start)
		return (false);
	for (;;) {
		r = uvm_core_range_find(tree, cursor);
		if (r == NULL || (accept != NULL && !accept(r)))
			return (false);
		if (r->end >= end)
			return (true);
		cursor = r->end + 1;
	}
}

/*
 * uvm_core_range_find_free - the first address in [base, base+length) that
 * holds `size` bytes aligned to `align` and overlaps no range of the tree.
 * 0 and the address in `out` when there is one, ENOSPC when there is none.
 *
 * The search starts at the base rounded up to the alignment and steps over
 * the ranges that are in the way: a range that ends at the end of the
 * address space, a window that wraps around it and a rounding that would
 * wrap around it all end the search.
 * Time: O(n log n) in the worst case, one lookup per range that is in the
 * way.  Space: O(1).
 */
int
uvm_core_range_find_free(struct uvm_core_range_tree *tree, uint64_t base, uint64_t length,
    uint64_t size, uint64_t align, uint64_t *out)
{
	uint64_t cur, limit;

	if (out == NULL || size == 0 || align == 0 || (align & (align - 1)) != 0)
		return (ENOSPC);
	if (base + length < base)
		return (ENOSPC);	/* the window wraps around the address space */
	limit = base + length;
	cur = uvm_core_range_roundup(base, align);
	if (cur < base)
		return (ENOSPC);	/* the alignment rounded past the end of the address space */
	while (cur + size <= limit && cur + size > cur) {
		struct uvm_core_range *r;
		uint64_t next_addr;

		r = uvm_core_range_find_overlap(tree, cur, cur + size - 1);
		if (r == NULL) {
			*out = cur;
			return (0);
		}
		if (r->end == UINT64_MAX)
			break;	/* the range runs to the end of the address space */
		next_addr = uvm_core_range_roundup(r->end + 1, align);
		if (next_addr <= cur)
			break;	/* the rounding wrapped: there is no room left */
		cur = next_addr;
	}
	return (ENOSPC);
}
