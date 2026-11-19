/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_ext_split.h - the arithmetic that splits an external GPU mapping
 * around an unmapped interval, for uvm-spec r1, 03-va-space-and-ranges.md
 * section 4.2 ("unmap splits partially overlapping mappings") and
 * 01-user-abi.md section 5.5 (UVM_UNMAP_EXTERNAL and
 * UVM_MAP_EXTERNAL_ALLOCATION: "Any existing mappings on that GPU
 * overlapping the range are unmapped first"; "Mappings partially
 * overlapping the range are split so only the overlap is removed").
 *
 * Pure function; tests/unit/ext_split_test.c compiles it in userland.
 *
 * The intervals are inclusive of both ends, as the mapping's [start, end]
 * of struct uvm_ext_mapping (sys/uvm.h) is.
 */

#ifndef _UVM_EXT_SPLIT_H_
#define	_UVM_EXT_SPLIT_H_

#include <sys/types.h>
#ifndef _KERNEL
#include <stdint.h>
#endif

/*
 * The plan for the mapping [m_start, m_end] against the unmapped interval
 * [u_start, u_end]:
 *
 *   overlaps   the two intervals intersect; only then is the mapping
 *              touched at all.
 *   keep_left  the part [m_start, u_start - 1] survives; it is empty when
 *              the interval reaches the mapping's first page.
 *   keep_right the part [u_end + 1, m_end] survives; it is empty when the
 *              interval reaches the mapping's last page.
 *
 * A part that is not kept has its bounds left at zero.
 */
struct uvm_ext_split_plan {
	int		overlaps;
	int		keep_left;
	int		keep_right;
	uint64_t	left_start;
	uint64_t	left_end;
	uint64_t	right_start;
	uint64_t	right_end;
};

/*
 * uvm_ext_split_plan - plan the split of the mapping [m_start, m_end] by
 * the unmapped interval [u_start, u_end], both inclusive.
 *
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_ext_split_plan(uint64_t m_start, uint64_t m_end, uint64_t u_start, uint64_t u_end,
    struct uvm_ext_split_plan *out)
{
	out->overlaps = (m_start <= u_end && u_start <= m_end) ? 1 : 0;
	out->keep_left = 0;
	out->keep_right = 0;
	out->left_start = 0;
	out->left_end = 0;
	out->right_start = 0;
	out->right_end = 0;
	if (!out->overlaps)
		return;
	/* m_start < u_start, so u_start - 1 does not wrap */
	if (m_start < u_start) {
		out->keep_left = 1;
		out->left_start = m_start;
		out->left_end = u_start - 1;
	}
	/* m_end > u_end, so u_end + 1 does not wrap */
	if (m_end > u_end) {
		out->keep_right = 1;
		out->right_start = u_end + 1;
		out->right_end = m_end;
	}
}

#endif /* _UVM_EXT_SPLIT_H_ */
