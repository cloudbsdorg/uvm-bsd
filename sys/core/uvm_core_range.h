/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_core_range.h - the neutral interval tree of the UVM core: the ranges
 * of a process's virtual address space, ordered by their first address.
 *
 * A range covers [start, end], both ends included.  The tree holds the
 * ranges of one address space and nothing else: it names no device and no
 * vendor, it takes no lock and it allocates nothing.  The caller owns the
 * memory of every range it puts in the tree, and it owns the locking; the
 * core is bookkeeping only.
 *
 * The header is usable from the kernel and from userland (like
 * sys/core/uvm_core_types.h), so that tests/unit/core_range_test.c can
 * compile it outside the kernel.  A kernel build compiles with -nostdinc,
 * so the standard integer and boolean headers are taken in userland only;
 * the kernel's own sys/types.h carries them.
 */

#ifndef _UVM_CORE_RANGE_H_
#define	_UVM_CORE_RANGE_H_

#include <sys/types.h>
#ifdef _KERNEL
#include <sys/_null.h>
#include <sys/errno.h>
#include <sys/stdint.h>
#else
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#endif
#include <sys/tree.h>

/* ---- one range of an address space ---- */

/*
 * A range of an address space.  The two ends are included: a range of one
 * page covers [0x1000, 0x1fff].  The node is the tree's own business; the
 * caller only fills in the two ends.
 */
struct uvm_core_range {
	RB_ENTRY(uvm_core_range) node;
	uint64_t	start, end;	/* inclusive */
};

/* the ranges of one address space, ordered by their first address */
RB_HEAD(uvm_core_range_tree, uvm_core_range);

/* ---- the tree ---- */

/*
 * uvm_core_range_tree_init - an empty tree.
 * Time: O(1).  Space: O(1).
 */
void	uvm_core_range_tree_init(struct uvm_core_range_tree *tree);

/*
 * uvm_core_range_find - the range that covers `addr`, or NULL when there is
 * none.
 * Time: O(log n).  Space: O(1).
 */
struct uvm_core_range *uvm_core_range_find(struct uvm_core_range_tree *tree, uint64_t addr);

/*
 * uvm_core_range_find_overlap - the first range that intersects
 * [start, end], or NULL when there is none.
 * Time: O(log n).  Space: O(1).
 */
struct uvm_core_range *uvm_core_range_find_overlap(struct uvm_core_range_tree *tree,
    uint64_t start, uint64_t end);

/*
 * uvm_core_range_insert - put `range` in the tree.  EEXIST when it would
 * overlap a range that is already there, 0 otherwise.  The tree keeps the
 * range itself; the caller keeps its memory.
 * Time: O(log n).  Space: O(1).
 */
int	uvm_core_range_insert(struct uvm_core_range_tree *tree, struct uvm_core_range *range);

/*
 * uvm_core_range_remove - take `range` out of the tree.  The range itself
 * is left alone; the caller owns its memory.
 * Time: O(log n).  Space: O(1).
 */
void	uvm_core_range_remove(struct uvm_core_range_tree *tree, struct uvm_core_range *range);

/*
 * uvm_core_range_first - the lowest range of the tree, or NULL when the
 * tree is empty.
 * Time: O(log n).  Space: O(1).
 */
struct uvm_core_range *uvm_core_range_first(struct uvm_core_range_tree *tree);

/*
 * uvm_core_range_next - the range that follows `range` in address order, or
 * NULL when `range` is the last one.
 * Time: O(log n).  Space: O(1).
 */
struct uvm_core_range *uvm_core_range_next(struct uvm_core_range_tree *tree,
    struct uvm_core_range *range);

/*
 * uvm_core_range_find_free - the first address in [base, base+length) that
 * holds `size` bytes aligned to `align` and overlaps no range of the tree.
 * 0 and the address in `out` when there is one, ENOSPC when there is none.
 * A window that wraps around the end of the address space does not fit.
 * Time: O(n log n) in the worst case, one lookup per range that is in the
 * way.  Space: O(1).
 */
int	uvm_core_range_find_free(struct uvm_core_range_tree *tree, uint64_t base, uint64_t length,
    uint64_t size, uint64_t align, uint64_t *out);

#endif /* _UVM_CORE_RANGE_H_ */
