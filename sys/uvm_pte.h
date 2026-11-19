/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_pte.h - GPU page table entry encoders and page-table geometry for
 * the two MMU formats of uvm-spec r1, 05-page-tables.md: version 2
 * (Pascal through Ada: 49-bit VA, four directory levels and a PTE level)
 * and version 3 (Hopper and Blackwell: 57-bit VA, five directory levels).
 * Pure functions; tests/unit/pte_encode_test.c compiles them in userland.
 *
 * Depth numbering is the spec's: depth 0 is the root.  The "dual" level is
 * the one whose 16-byte entries hold a big (64K-page table or 2M PTE) half
 * and a small (4K-page table) half.  Addresses given to the encoders are
 * GPU-visible bus addresses for system memory and GPU physical addresses
 * for video memory.
 */

#ifndef _UVM_PTE_H_
#define	_UVM_PTE_H_

#include "uvm_hal_methods.h"	/* uvm_aperture_t and the hardware aperture codes */

#define	UVM_PAGE_SIZE_4K	0x1000ULL
#define	UVM_PAGE_SIZE_64K	0x10000ULL
#define	UVM_PAGE_SIZE_2M	0x200000ULL
#define	UVM_PAGE_SIZE_512M	0x20000000ULL

typedef enum {
	UVM_PROT_NONE = 0,
	UVM_PROT_READ_ONLY = 1,
	UVM_PROT_READ_WRITE = 2,
	UVM_PROT_READ_WRITE_ATOMIC = 3
} uvm_prot_t;

/* kind field: "generic memory" from Turing on; "SKED message" for dynamic parallelism */
#define	UVM_PTE_KIND_GENERIC	6ULL
#define	UVM_PTE_KIND_SKED	0xFULL

static inline uint64_t
uvm_pte_hw_aperture(uvm_aperture_t aperture)
{
	if (aperture == UVM_APERTURE_VID)
		return (UVM_HW_APERTURE_VID);
	if (UVM_APERTURE_IS_PEER(aperture))
		return (UVM_HW_APERTURE_PEER);
	return (UVM_HW_APERTURE_SYS_COHERENT);
}

/* ---- version 3 (05 section 1.2, PCF table 12 section 7) ---- */

/* PDE aperture codes: 0 invalid, 1 video, 2 coherent system, 3 non-coherent */
static inline uint64_t
uvm_pde_hw_aperture(uvm_aperture_t aperture)
{
	return (aperture == UVM_APERTURE_VID ? 1ULL : 2ULL);
}

/* valid PDE: "is PTE" clear, aperture 2:1, PCF 5:3 = 1 (valid, uncached, ATS allowed), address 51:12 */
static inline uint64_t
uvm_pte3_pde(uint64_t table_addr, uvm_aperture_t aperture)
{
	return ((table_addr & 0x000FFFFFFFFFF000ULL) | (uvm_pde_hw_aperture(aperture) << 1) | (1ULL << 3));
}

static inline uint64_t
uvm_pte3_pde_invalid(void)
{
	return (0);
}

/*
 * dual PDE: word 0 is the big half (address 51:8), word 1 the small half
 * (aperture 66:65, PCF 69:67, address 115:76 in the 128-bit entry).  A half
 * with no table is written entirely invalid (0).
 */
static inline void
uvm_pte3_dual_pde(uint64_t *entry, uint64_t big_addr, uvm_aperture_t big_ap, uint64_t small_addr,
    uvm_aperture_t small_ap)
{
	entry[0] = big_addr == 0 ? 0 :
	    (big_addr & 0x000FFFFFFFFFFF00ULL) | (uvm_pde_hw_aperture(big_ap) << 1) | (1ULL << 3);
	entry[1] = small_addr == 0 ? 0 :
	    (small_addr & 0x000FFFFFFFFFF000ULL) | (uvm_pde_hw_aperture(small_ap) << 1) | (1ULL << 3);
}

/*
 * PCF of a valid PTE: bit 0 uncached, bit 1 privileged, bit 2 read-only,
 * bit 3 atomics disabled, bit 4 access counting disabled.  UVM maps system
 * memory and any "force uncached" request uncached, read-only mappings
 * without atomics, atomics only for read/write/atomic protection, access
 * counting always enabled, never privileged.
 */
static inline uint64_t
uvm_pte3_pcf(uvm_aperture_t aperture, uvm_prot_t prot, int force_uncached)
{
	uint64_t pcf = 0;

	if (aperture != UVM_APERTURE_VID || force_uncached)
		pcf |= 1ULL << 0;
	if (prot == UVM_PROT_READ_ONLY)
		pcf |= 1ULL << 2;
	if (prot != UVM_PROT_READ_WRITE_ATOMIC)
		pcf |= 1ULL << 3;
	return (pcf);
}

/* valid PTE: bit 0, aperture 2:1, PCF 7:3, kind 11:8, address vid 39:12 / sys and peer 51:12, peer id 63:61 */
static inline uint64_t
uvm_pte3_pte_kind(uint64_t addr, uvm_aperture_t aperture, uvm_prot_t prot, int force_uncached,
    uint64_t kind)
{
	uint64_t pte = 1ULL | (uvm_pte_hw_aperture(aperture) << 1) |
	    (uvm_pte3_pcf(aperture, prot, force_uncached) << 3) | (kind << 8);

	if (aperture == UVM_APERTURE_VID)
		pte |= addr & 0x000000FFFFFFF000ULL;
	else
		pte |= addr & 0x000FFFFFFFFFF000ULL;
	if (UVM_APERTURE_IS_PEER(aperture))
		pte |= (uint64_t)(UVM_APERTURE_PEER_ID(aperture) & 7u) << 61;
	return (pte);
}

static inline uint64_t
uvm_pte3_pte(uint64_t addr, uvm_aperture_t aperture, uvm_prot_t prot, int force_uncached, int unused)
{
	(void)unused;
	return (uvm_pte3_pte_kind(addr, aperture, prot, force_uncached, UVM_PTE_KIND_GENERIC));
}

/* SKED reflected PTE: valid, kind 0xF, no address */
static inline uint64_t
uvm_pte3_pte_sked(void)
{
	return (1ULL | (UVM_PTE_KIND_SKED << 8));
}

/* invalid-PTE PCF codes: 0 invalid, 1 sparse, 2 mapping nowhere, 3 no valid 4K page */
static inline uint64_t uvm_pte3_pte_invalid(void) { return (0); }
static inline uint64_t uvm_pte3_pte_sparse(void) { return (1ULL << 3); }
static inline uint64_t uvm_pte3_pte_unmapped_big(void) { return (3ULL << 3); }

/* ---- version 2 (05 section 1.1) ---- */

/* PDE: bit 0 clear, aperture 2:1, volatile bit 3 set, address (addr >> 12) at bit 8 */
static inline uint64_t
uvm_pte2_pde(uint64_t table_addr, uvm_aperture_t aperture)
{
	return (((table_addr >> 12) << 8) | (uvm_pde_hw_aperture(aperture) << 1) | (1ULL << 3));
}

static inline uint64_t
uvm_pte2_pde_invalid(void)
{
	return (0);
}

/* dual PDE: word 0 big half, address (addr >> 8) at bit 4; word 1 small half, address (addr >> 12) at bit 8 */
static inline void
uvm_pte2_dual_pde(uint64_t *entry, uint64_t big_addr, uvm_aperture_t big_ap, uint64_t small_addr,
    uvm_aperture_t small_ap)
{
	entry[0] = big_addr == 0 ? 0 :
	    ((big_addr >> 8) << 4) | (uvm_pde_hw_aperture(big_ap) << 1) | (1ULL << 3);
	entry[1] = small_addr == 0 ? 0 :
	    ((small_addr >> 12) << 8) | (uvm_pde_hw_aperture(small_ap) << 1) | (1ULL << 3);
}

/*
 * PTE: valid 0, aperture 2:1 (0 video, 1 peer, 2 coherent system), volatile
 * 3, privilege 5, read-only 6, atomic-disable 7, address (addr >> 12) at bit
 * 8 (46 bits for system memory, 25 for video with the overflow in the
 * comptagline field 63:44), peer id 35:33, kind 43:36.
 */
static inline uint64_t
uvm_pte2_pte_kind(uint64_t addr, uvm_aperture_t aperture, uvm_prot_t prot, int force_uncached, uint64_t kind)
{
	uint64_t pfn = addr >> 12;
	uint64_t pte = 1ULL | (uvm_pte_hw_aperture(aperture) << 1) | (kind << 36);

	if (aperture != UVM_APERTURE_VID || force_uncached)
		pte |= 1ULL << 3;
	if (prot == UVM_PROT_READ_ONLY)
		pte |= 1ULL << 6;
	if (prot != UVM_PROT_READ_WRITE_ATOMIC)
		pte |= 1ULL << 7;
	if (aperture == UVM_APERTURE_VID) {
		pte |= (pfn & 0x1FFFFFFULL) << 8;
		pte |= ((pfn >> 25) & 0xFFFFFULL) << 44;
	} else {
		pte |= (pfn & 0x3FFFFFFFFFFFULL) << 8;
	}
	if (UVM_APERTURE_IS_PEER(aperture))
		pte |= (uint64_t)(UVM_APERTURE_PEER_ID(aperture) & 7u) << 33;
	return (pte);
}

static inline uint64_t
uvm_pte2_pte(uint64_t addr, uvm_aperture_t aperture, uvm_prot_t prot, int force_uncached)
{
	return (uvm_pte2_pte_kind(addr, aperture, prot, force_uncached, UVM_PTE_KIND_GENERIC));
}

/* special version-2 patterns */
static inline uint64_t uvm_pte2_pte_invalid(void) { return (0); }
static inline uint64_t uvm_pte2_pte_unmapped_big(void) { return (1ULL << 5); }	/* privilege only */
static inline uint64_t uvm_pte2_pte_sparse(void) { return (1ULL << 3); }	/* volatile only */
static inline uint64_t uvm_pte2_pte_sked(void) { return (1ULL | (UVM_PTE_KIND_SKED << 36)); }

/* ---- geometry (05 section 1 table) ---- */

/* depths: v3 has PDE4..PDE0 and the PTE level (6); v2 PDE3..PDE0 and the PTE level (5) */
static inline uint32_t
uvm_mmu_depth_count(uint32_t mmu_version)
{
	return (mmu_version == 3 ? 6 : 5);
}

/* the depth of the dual (big/small) level */
static inline uint32_t
uvm_mmu_dual_depth(uint32_t mmu_version)
{
	return (mmu_version == 3 ? 4 : 3);
}

/* the depth whose entries are PTEs for a page size */
static inline uint32_t
uvm_mmu_pte_depth(uint32_t mmu_version, uint64_t page_size)
{
	uint32_t dual = uvm_mmu_dual_depth(mmu_version);

	if (page_size == UVM_PAGE_SIZE_512M)
		return (dual - 1);
	if (page_size == UVM_PAGE_SIZE_2M)
		return (dual);
	return (dual + 1);
}

/* the lowest VA bit an entry at `depth` indexes (the span of one entry is 1 << this) */
static inline uint32_t
uvm_mmu_entry_shift(uint32_t mmu_version, uint32_t depth)
{
	uint32_t dual = uvm_mmu_dual_depth(mmu_version);

	if (depth > dual)		/* PTE level: 4K pages by default */
		return (12);
	if (depth == dual)		/* the dual level indexes 8 bits, 28:21 */
		return (21);
	/* each level above the dual one adds 9 bits: PDE1 29, PDE2 38, PDE3 47, PDE4 56 */
	return (29 + 9 * (dual - 1 - depth));
}

static inline uint64_t
uvm_mmu_entry_span(uint32_t mmu_version, uint32_t depth)
{
	return (1ULL << uvm_mmu_entry_shift(mmu_version, depth));
}

/* the index of `va` in the table at `depth`; at the PTE level the page size picks 4K or 64K entries */
static inline uint32_t
uvm_mmu_index(uint32_t mmu_version, uint32_t depth, uint64_t va, uint64_t page_size)
{
	uint32_t dual = uvm_mmu_dual_depth(mmu_version);

	if (depth > dual) {
		if (page_size == UVM_PAGE_SIZE_64K)
			return ((uint32_t)((va >> 16) & 0x1F));
		return ((uint32_t)((va >> 12) & 0x1FF));
	}
	if (depth == dual)
		return ((uint32_t)((va >> 21) & 0xFF));
	if (depth == 0)
		return ((uint32_t)((va >> uvm_mmu_entry_shift(mmu_version, 0)) & (mmu_version == 3 ? 0x1 : 0x3)));
	return ((uint32_t)((va >> uvm_mmu_entry_shift(mmu_version, depth)) & 0x1FF));
}

/* entries in the table at `depth` (page size matters at the PTE level only) */
static inline uint32_t
uvm_mmu_entries(uint32_t mmu_version, uint32_t depth, uint64_t page_size)
{
	uint32_t dual = uvm_mmu_dual_depth(mmu_version);

	if (depth == 0)
		return (mmu_version == 3 ? 2 : 4);
	if (depth == dual)
		return (256);
	if (depth > dual)
		return (page_size == UVM_PAGE_SIZE_64K ? 32 : 512);
	return (512);
}

static inline uint32_t
uvm_mmu_entry_size(uint32_t mmu_version, uint32_t depth)
{
	return (depth == uvm_mmu_dual_depth(mmu_version) ? 16 : 8);
}

static inline uint32_t
uvm_mmu_table_bytes(uint32_t mmu_version, uint32_t depth, uint64_t page_size)
{
	return (uvm_mmu_entries(mmu_version, depth, page_size) * uvm_mmu_entry_size(mmu_version, depth));
}

#endif /* _UVM_PTE_H_ */
