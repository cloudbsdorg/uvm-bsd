/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pte_encode_test.c - the page table entry encoders of sys/uvm_pte.h
 * produce the bit patterns of uvm-spec r1, 05-page-tables.md sections 1.1
 * (MMU version 2) and 1.2 (MMU version 3) and the PCF table of
 * 12-hal-per-generation.md section 7, and the geometry helpers give the
 * level indexes and table sizes of section 1.
 *
 * Red first: compiled before the header existed.
 */

#include <stdio.h>

#include "uvm_pte.h"

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
	uint64_t pte, pde[2];

	/* ---- version 3 ---- */
	/* PDE: bit 0 clear, aperture 2:1, PCF 5:3 = 1 (valid, uncached, ATS allowed), address 51:12 */
	pde[0] = uvm_pte3_pde(0x0000123456789000ULL, UVM_APERTURE_SYS);
	CHECK(pde[0] == (0x0000123456789000ULL | (2ULL << 1) | (1ULL << 3)), "v3 PDE sysmem %#llx",
	    (unsigned long long)pde[0]);
	pde[0] = uvm_pte3_pde(0x0000000080000000ULL, UVM_APERTURE_VID);
	CHECK(pde[0] == (0x80000000ULL | (1ULL << 1) | (1ULL << 3)), "v3 PDE vidmem");
	CHECK(uvm_pte3_pde_invalid() == 0, "v3 invalid PDE is all zero");
	/* dual PDE: word 0 big half address 51:8 ((addr >> 8) << 8), word 1 small half aperture 66:65 PCF 69:67 address 115:76 */
	uvm_pte3_dual_pde(pde, 0x0000123456789000ULL, UVM_APERTURE_SYS, 0x0000ABCDEF000000ULL, UVM_APERTURE_VID);
	CHECK(pde[0] == (0x0000123456789000ULL | (2ULL << 1) | (1ULL << 3)), "v3 dual big half %#llx",
	    (unsigned long long)pde[0]);
	CHECK(pde[1] == (((0x0000ABCDEF000000ULL >> 12) << 12) | (1ULL << 1) | (1ULL << 3)),
	    "v3 dual small half %#llx", (unsigned long long)pde[1]);
	/* a dual PDE with no small table: small half entirely invalid */
	uvm_pte3_dual_pde(pde, 0x1000, UVM_APERTURE_VID, 0, UVM_APERTURE_VID);
	CHECK(pde[1] == 0, "v3 dual: absent small half is 0");

	/* PTE: valid bit 0, aperture 2:1, PCF 7:3, kind 11:8 = 6, address vid 39:12 / sys 51:12, peer 63:61 */
	pte = uvm_pte3_pte(0x0000123456789000ULL, UVM_APERTURE_SYS, UVM_PROT_READ_WRITE_ATOMIC, 0, 0);
	/* sysmem is always uncached (PCF bit 0) -> PCF 0x01 */
	CHECK(pte == (0x0000123456789000ULL | 1ULL | (2ULL << 1) | (0x01ULL << 3) | (6ULL << 8)),
	    "v3 PTE sysmem RWA %#llx", (unsigned long long)pte);
	pte = uvm_pte3_pte(0x80000000ULL, UVM_APERTURE_VID, UVM_PROT_READ_WRITE_ATOMIC, 0, 0);
	CHECK(pte == (0x80000000ULL | 1ULL | (0x00ULL << 3) | (6ULL << 8)), "v3 PTE vidmem RWA cached");
	pte = uvm_pte3_pte(0x80000000ULL, UVM_APERTURE_VID, UVM_PROT_READ_WRITE_ATOMIC, 1, 0);
	CHECK(((pte >> 3) & 0x1f) == 0x01, "v3 PTE vidmem forced uncached");
	pte = uvm_pte3_pte(0x80000000ULL, UVM_APERTURE_VID, UVM_PROT_READ_WRITE, 0, 0);
	CHECK(((pte >> 3) & 0x1f) == 0x08, "v3 PTE RW no atomics");
	pte = uvm_pte3_pte(0x80000000ULL, UVM_APERTURE_VID, UVM_PROT_READ_ONLY, 0, 0);
	CHECK(((pte >> 3) & 0x1f) == 0x0C, "v3 PTE RO (no atomics)");
	pte = uvm_pte3_pte(0x1000, UVM_APERTURE_SYS, UVM_PROT_READ_ONLY, 0, 0);
	CHECK(((pte >> 3) & 0x1f) == 0x0D, "v3 PTE RO sysmem uncached");
	/* peer aperture 1 with peer id bits 63:61 */
	pte = uvm_pte3_pte(0x1000, UVM_APERTURE_PEER0 + 5, UVM_PROT_READ_WRITE_ATOMIC, 0, 0);
	CHECK(((pte >> 1) & 3) == 1 && (pte >> 61) == 5, "v3 PTE peer 5");
	/* kind override: SKED reflected 0xF without address */
	pte = uvm_pte3_pte_sked(0);
	CHECK(pte == (1ULL | (0xFULL << 8)), "v3 SKED PTE %#llx", (unsigned long long)pte);
	/* without video memory: aperture 3 (system memory, non-coherent), everything else the same */
	pte = uvm_pte3_pte_sked(1);
	CHECK(pte == (1ULL | (3ULL << 1) | (0xFULL << 8)), "v3 SKED PTE, no video memory %#llx", (unsigned long long)pte);
	/* invalid PTE patterns: PCF of an invalid entry */
	CHECK(uvm_pte3_pte_invalid() == 0, "v3 invalid PTE");
	CHECK(uvm_pte3_pte_sparse() == (1ULL << 3), "v3 sparse PTE PCF 1");
	CHECK(uvm_pte3_pte_unmapped_big() == (3ULL << 3), "v3 unmapped big PTE PCF 3");

	/* ---- version 2 ---- */
	/* PDE: bit 0 clear, aperture 2:1 (1 vid, 2 sys), volatile bit 3, address: sys 53:8, vid 32:8 as (addr>>12)<<8 */
	pde[0] = uvm_pte2_pde(0x0000123456789000ULL, UVM_APERTURE_SYS);
	CHECK(pde[0] == (((0x0000123456789000ULL >> 12) << 8) | (2ULL << 1) | (1ULL << 3)), "v2 PDE sys %#llx",
	    (unsigned long long)pde[0]);
	pde[0] = uvm_pte2_pde(0x80000000ULL, UVM_APERTURE_VID);
	CHECK(pde[0] == (((0x80000000ULL >> 12) << 8) | (1ULL << 1) | (1ULL << 3)), "v2 PDE vid");
	uvm_pte2_dual_pde(pde, 0x0000123456789000ULL, UVM_APERTURE_SYS, 0x80000000ULL, UVM_APERTURE_VID);
	/* big half address 53:4 as (addr>>8)<<4; small half in word 1: aperture 66:65 -> bits 2:1, volatile 67 -> bit 3, address 117:72 -> (addr>>12)<<8 */
	CHECK(pde[0] == (((0x0000123456789000ULL >> 8) << 4) | (2ULL << 1) | (1ULL << 3)), "v2 dual big %#llx",
	    (unsigned long long)pde[0]);
	CHECK(pde[1] == (((0x80000000ULL >> 12) << 8) | (1ULL << 1) | (1ULL << 3)), "v2 dual small %#llx",
	    (unsigned long long)pde[1]);
	/* PTE: valid 0, aperture 2:1 (0 vid 1 peer 2 sys), volatile 3, priv 5, RO 6, atomic-disable 7,
	 * address sys 53:8 / vid 32:8 as (addr>>12)<<8, kind word1 bits 11:4 = 6 (generic) -> bits 43:36 */
	pte = uvm_pte2_pte(0x0000123456789000ULL, UVM_APERTURE_SYS, UVM_PROT_READ_WRITE_ATOMIC, 0);
	CHECK(pte == (((0x0000123456789000ULL >> 12) << 8) | 1ULL | (2ULL << 1) | (1ULL << 3) | (6ULL << 36)),
	    "v2 PTE sys RWA %#llx", (unsigned long long)pte);
	pte = uvm_pte2_pte(0x80000000ULL, UVM_APERTURE_VID, UVM_PROT_READ_ONLY, 0);
	CHECK((pte & (1ULL << 3)) == 0 && (pte & (1ULL << 6)) && (pte & (1ULL << 7)), "v2 PTE vid RO: cached, RO, no atomics");
	pte = uvm_pte2_pte(0x80000000ULL, UVM_APERTURE_VID, UVM_PROT_READ_WRITE, 1);
	CHECK((pte & (1ULL << 3)) && (pte & (1ULL << 6)) == 0 && (pte & (1ULL << 7)), "v2 PTE RW uncached no atomics");
	/* video address overflow above bit 32 goes to the comptagline field (word 1 bits 31:12 -> 63:44) */
	pte = uvm_pte2_pte(0x0000012345678000ULL, UVM_APERTURE_VID, UVM_PROT_READ_WRITE_ATOMIC, 0);
	CHECK(((pte >> 8) & 0x1FFFFFFULL) == (0x0000012345678000ULL >> 12 & 0x1FFFFFFULL) &&
	    ((pte >> 44) & 0xFFFFFULL) == ((0x0000012345678000ULL >> 12) >> 25), "v2 vid overflow to comptagline");
	pte = uvm_pte2_pte(0x1000, UVM_APERTURE_PEER0 + 3, UVM_PROT_READ_WRITE_ATOMIC, 0);
	CHECK(((pte >> 1) & 3) == 1 && ((pte >> 33) & 7) == 3, "v2 peer id 35:33");
	CHECK(uvm_pte2_pte_unmapped_big() == (1ULL << 5), "v2 unmapped big PTE: privilege only");
	CHECK(uvm_pte2_pte_sparse() == (1ULL << 3), "v2 sparse PTE: volatile only");
	CHECK(uvm_pte2_pte_sked() == (1ULL | (0xFULL << 36)), "v2 SKED PTE");

	/* ---- geometry ---- */
	CHECK(uvm_mmu_depth_count(3) == 6 && uvm_mmu_depth_count(2) == 5, "depth counts");
	CHECK(uvm_mmu_pte_depth(3, UVM_PAGE_SIZE_4K) == 5 && uvm_mmu_pte_depth(3, UVM_PAGE_SIZE_64K) == 5 &&
	    uvm_mmu_pte_depth(3, UVM_PAGE_SIZE_2M) == 4 && uvm_mmu_pte_depth(3, UVM_PAGE_SIZE_512M) == 3,
	    "v3 PTE depths");
	CHECK(uvm_mmu_pte_depth(2, UVM_PAGE_SIZE_4K) == 4 && uvm_mmu_pte_depth(2, UVM_PAGE_SIZE_2M) == 3 &&
	    uvm_mmu_pte_depth(2, UVM_PAGE_SIZE_512M) == 2, "v2 PTE depths");
	/* v3 index bits: depth 0 bit 56 (1 bit), 1: 55:47, 2: 46:38, 3: 37:29, 4: 28:21 (8), 5: 20:12 (9) or 20:16 (5) */
	CHECK(uvm_mmu_index(3, 0, 1ULL << 56, UVM_PAGE_SIZE_4K) == 1 && uvm_mmu_index(3, 0, 0x7FFFFFFFFFFFULL, UVM_PAGE_SIZE_4K) == 0,
	    "v3 root index");
	CHECK(uvm_mmu_index(3, 1, 0x00FF800000000000ULL, UVM_PAGE_SIZE_4K) == 0x1FF, "v3 depth 1 index");
	CHECK(uvm_mmu_index(3, 4, 0x1FE00000ULL, UVM_PAGE_SIZE_4K) == 0xFF, "v3 depth 4 (dual) index");
	CHECK(uvm_mmu_index(3, 5, 0x1FF000ULL, UVM_PAGE_SIZE_4K) == 0x1FF, "v3 4K PTE index");
	CHECK(uvm_mmu_index(3, 5, 0x1F0000ULL, UVM_PAGE_SIZE_64K) == 0x1F, "v3 64K PTE index");
	CHECK(uvm_mmu_index(2, 0, 0x1800000000000ULL, UVM_PAGE_SIZE_4K) == 3, "v2 root index (2 bits, 48:47)");
	CHECK(uvm_mmu_index(2, 3, 0x1FE00000ULL, UVM_PAGE_SIZE_4K) == 0xFF && uvm_mmu_index(2, 4, 0xF0000ULL, UVM_PAGE_SIZE_64K) == 0xF,
	    "v2 dual and 64K indexes");
	/* entry counts and table sizes: root 2 (v3) / 4 (v2), 512 at single levels, 256 dual (16-byte), 32 for 64K PTEs */
	CHECK(uvm_mmu_entries(3, 0, 0) == 2 && uvm_mmu_entries(2, 0, 0) == 4 && uvm_mmu_entries(3, 2, 0) == 512 &&
	    uvm_mmu_entries(3, 4, 0) == 256 && uvm_mmu_entries(3, 5, UVM_PAGE_SIZE_64K) == 32 &&
	    uvm_mmu_entries(3, 5, UVM_PAGE_SIZE_4K) == 512, "entry counts");
	CHECK(uvm_mmu_entry_size(3, 4) == 16 && uvm_mmu_entry_size(3, 5) == 8 && uvm_mmu_entry_size(2, 3) == 16 &&
	    uvm_mmu_entry_size(2, 4) == 8, "entry sizes");
	CHECK(uvm_mmu_table_bytes(3, 5, UVM_PAGE_SIZE_64K) == 256 && uvm_mmu_table_bytes(3, 4, 0) == 4096 &&
	    uvm_mmu_table_bytes(3, 0, 0) == 16, "table sizes");
	/* the VA span one entry covers */
	CHECK(uvm_mmu_entry_span(3, 4) == (2ULL << 20) && uvm_mmu_entry_span(3, 3) == (1ULL << 29) &&
	    uvm_mmu_entry_span(3, 0) == (1ULL << 56) && uvm_mmu_entry_span(2, 0) == (1ULL << 47), "entry spans");
	CHECK(uvm_mmu_dual_depth(3) == 4 && uvm_mmu_dual_depth(2) == 3, "dual depths");

	if (fails == 0)
		printf("pte_encode_test: PASS\n");
	return (fails != 0);
}
