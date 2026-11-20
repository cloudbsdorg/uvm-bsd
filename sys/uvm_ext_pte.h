/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_ext_pte.h - sizing and reading the PTE buffer that RM fills for an
 * external allocation (nvUvmInterfaceGetExternalAllocPtes, uvm-spec r1
 * 02 section 3.8).  RM writes one entry per page in the size of the page
 * tree entry at that page size: 8 bytes at the PTE levels (4K, 64K) and 16
 * bytes for a 2M page, whose PTE sits in the dual level (05 section 1).  It
 * reports the size in pteSize, and answers BUFFER_TOO_SMALL when the buffer
 * is sized for 8 bytes per 2M page.  The first word of a 16-byte entry is
 * the PTE (the page tree writer adds the empty small half).
 *
 * Pure functions; tests/unit/ext_pte_test.c compiles them in userland.
 */

#ifndef _UVM_EXT_PTE_H_
#define	_UVM_EXT_PTE_H_

#include <sys/types.h>
#ifndef _KERNEL
#include <stdint.h>
#endif

/* bytes of one entry RM writes for a mapping page size.  Time: O(1). */
static inline uint32_t
uvm_ext_pte_entry_size(uint64_t page_size)
{
	return (page_size >= ((uint64_t)2 << 20) ? 16 : 8);
}

/* the pteSize RM reported is the one the page size implies.  Time: O(1). */
static inline int
uvm_ext_pte_size_ok(uint64_t page_size, uint32_t pte_size)
{
	return (pte_size == uvm_ext_pte_entry_size(page_size));
}

/*
 * uvm_ext_pte_compact - squeeze `count` entries of `entry_size` bytes in
 * place to one 64-bit PTE each (the first word of the entry).
 *
 * Time: O(count).  Space: O(1).
 */
static inline void
uvm_ext_pte_compact(uint64_t *buf, uint32_t entry_size, uint64_t count)
{
	uint64_t i, stride = entry_size / sizeof(uint64_t);

	if (stride <= 1)
		return;
	for (i = 1; i < count; i++)
		buf[i] = buf[i * stride];
}

/*
 * uvm_sked_page_size - the page size of a dynamic-parallelism region
 * (UVM_MAP_DYNAMIC_PARALLELISM_REGION).  The spec says 64K alignment, but
 * the user-mode driver asks for a 4096-byte region at a 64K-aligned base
 * (live, CTX999-REPORT; Q-012): 64K pages when base and length are both 64K
 * aligned, else 4K pages; 0 when either is not 4K aligned or length is 0.
 * Time: O(1).
 */
static inline uint64_t
uvm_sked_page_size(uint64_t base, uint64_t length)
{
	if (length == 0 || (base & 0xfff) != 0 || (length & 0xfff) != 0)
		return (0);
	if ((base & 0xffff) == 0 && (length & 0xffff) == 0)
		return (65536);
	return (4096);
}

#endif /* _UVM_EXT_PTE_H_ */
