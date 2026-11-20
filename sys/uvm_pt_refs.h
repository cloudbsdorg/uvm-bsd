/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_pt_refs.h - the reference rule of the page-table tree (uvm-spec r1
 * 05 section 2).  A table holds one reference per child table and per PTE
 * range handed out; the root also holds the tree's own reference (1).  A
 * reference is dropped the same way at every depth, the root included: the
 * root is never freed by a drop, but it must be counted down, or the
 * references that its freed children held stay on it for ever.
 */

#ifndef UVM_PT_REFS_H
#define UVM_PT_REFS_H

#ifndef _KERNEL
#include <stdbool.h>
#endif

/*
 * uvm_pt_ref_release - drop one reference on a table; true when the table
 * has none left and must be unlinked and freed (never for the root).
 */
static inline bool
uvm_pt_ref_release(unsigned *refs, bool is_root)
{
	if (--*refs > 0)
		return (false);
	return (!is_root);
}

#endif /* UVM_PT_REFS_H */
