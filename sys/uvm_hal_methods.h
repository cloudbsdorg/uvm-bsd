/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_hal_methods.h - pure encoders for the GPU method stream, GPFIFO
 * entries and fault-buffer entries, and the per-generation table of
 * uvm-spec r1, 12-hal-per-generation.md.  Everything here is a function of
 * its arguments with no kernel dependency, so tests/unit/hal_encode_test.c
 * compiles it in userland.
 *
 * Encodings are the Hopper/Blackwell ones of 04-channels-and-copy-engine.md
 * section 1 (host class 0xC96F/0xCA6F, copy class 0xC9B5/0xCAB5), the
 * generation this implementation supports first.  Where an encoding differs
 * by generation (TLB level base, cancel engine-id width) the caller passes
 * the generation's value from the table at the end of this file.
 *
 * Method stream words are little-endian 32-bit; each encoder writes into a
 * caller-provided array and returns the number of words written.  Callers
 * size their buffers from the UVM_*_MAX_WORDS constants.
 */

#ifndef _UVM_HAL_METHODS_H_
#define	_UVM_HAL_METHODS_H_

#include <sys/types.h>
#ifndef _KERNEL
#include <stdint.h>
#include <stddef.h>
#endif

/* 1.1 method header: opcode 31:29, count 28:16, subchannel 15:13, address/4 11:0 */
#define	UVM_METHOD_INCR		1u
#define	UVM_METHOD_NONINCR	3u
#define	UVM_METHOD_MAX_COUNT	8191u
#define	UVM_SUBCH_HOST		0u
#define	UVM_SUBCH_CE		4u
#define	UVM_SUBCH_SW		5u

/* host method addresses (bytes) */
#define	UVM_HOST_SET_OBJECT	0x0000u
#define	UVM_HOST_NOP		0x0008u
#define	UVM_HOST_MEM_OP_A	0x0028u
#define	UVM_HOST_MEM_OP_B	0x002Cu
#define	UVM_HOST_MEM_OP_C	0x0030u
#define	UVM_HOST_MEM_OP_D	0x0034u
#define	UVM_HOST_SEM_ADDR_LO	0x005Cu
#define	UVM_HOST_SEM_ADDR_HI	0x0060u
#define	UVM_HOST_SEM_PAYLOAD_LO	0x0064u
#define	UVM_HOST_SEM_PAYLOAD_HI	0x0068u
#define	UVM_HOST_SEM_EXECUTE	0x006Cu
#define	UVM_HOST_WFI		0x0078u

/* copy engine method addresses (bytes), subchannel 4 */
#define	UVM_CE_SET_SEMAPHORE_A		0x0240u
#define	UVM_CE_SET_SEMAPHORE_B		0x0244u
#define	UVM_CE_SET_SEMAPHORE_PAYLOAD	0x0248u
#define	UVM_CE_SET_SRC_PHYS_MODE	0x0260u
#define	UVM_CE_SET_DST_PHYS_MODE	0x0264u
#define	UVM_CE_LAUNCH_DMA		0x0300u
#define	UVM_CE_OFFSET_IN_UPPER		0x0400u
#define	UVM_CE_OFFSET_IN_LOWER		0x0404u
#define	UVM_CE_OFFSET_OUT_UPPER		0x0408u
#define	UVM_CE_OFFSET_OUT_LOWER		0x040Cu
#define	UVM_CE_LINE_LENGTH_IN		0x0418u
#define	UVM_CE_LINE_COUNT		0x041Cu
#define	UVM_CE_SET_MEMORY_SCRUB_PARAMETERS 0x06FCu
#define	UVM_CE_SET_REMAP_CONST_A	0x0700u
#define	UVM_CE_SET_REMAP_CONST_B	0x0704u
#define	UVM_CE_SET_REMAP_COMPONENTS	0x0708u

/* software class 0xC076 methods (clear faulted), subchannel 5 */
#define	UVM_SW_CLASS			0xC076u
#define	UVM_SW_CLEAR_FAULTED_A		0x0110u
#define	UVM_SW_CLEAR_FAULTED_B		0x0114u

/* MEM_OP_D operations */
#define	UVM_MEM_OP_MEMBAR			5u
#define	UVM_MEM_OP_TLB_INVALIDATE		9u
#define	UVM_MEM_OP_TLB_INVALIDATE_TARGETED	10u
#define	UVM_MEM_OP_L2_PEERMEM_INVALIDATE	13u
#define	UVM_MEM_OP_L2_SYSMEM_INVALIDATE		14u

/* the biggest single piece a copy engine launch moves */
#define	UVM_CE_MAX_PIECE	0xFFFFFFFFull

typedef enum { UVM_MEMBAR_NONE = 0, UVM_MEMBAR_GPU = 1, UVM_MEMBAR_SYS = 2 } uvm_membar_t;
typedef enum { UVM_REPLAY_START = 1, UVM_REPLAY_START_ACK_ALL = 2 } uvm_replay_t;
typedef enum { UVM_CANCEL_ACCESS_WRITE_ATOMIC = 6, UVM_CANCEL_ACCESS_ALL = 7 } uvm_cancel_access_t;

/*
 * GPU physical apertures.  PEER0..PEER7 carry the peer id; the PTE and
 * copy-engine encoders turn them into the hardware's aperture codes.
 */
typedef enum {
	UVM_APERTURE_VID = 0,
	UVM_APERTURE_SYS = 1,
	UVM_APERTURE_PEER0 = 2,
	UVM_APERTURE_PEER_MAX = UVM_APERTURE_PEER0 + 8
} uvm_aperture_t;

#define	UVM_APERTURE_IS_PEER(a)	((a) >= UVM_APERTURE_PEER0 && (a) < UVM_APERTURE_PEER_MAX)
#define	UVM_APERTURE_PEER_ID(a)	((unsigned)((a) - UVM_APERTURE_PEER0))

/* an address the copy engine takes: virtual in the internal address space, or physical */
struct uvm_gpu_address {
	uint64_t	address;
	uint8_t		is_virtual;
	uvm_aperture_t	aperture;	/* meaningful when physical */
};

/* a page directory base: physical address and aperture */
struct uvm_pdb {
	uint64_t	phys;
	uint8_t		vidmem;
};

/* hardware aperture codes shared by PDB fields, PTEs and the fault entry */
#define	UVM_HW_APERTURE_VID		0u
#define	UVM_HW_APERTURE_PEER		1u
#define	UVM_HW_APERTURE_SYS_COHERENT	2u
#define	UVM_HW_APERTURE_SYS_NONCOHERENT	3u

static inline uint32_t
uvm_method_header(uint32_t opcode, uint32_t subch, uint32_t addr, uint32_t count)
{
	return ((opcode & 7u) << 29) | ((count & 0x1FFFu) << 16) | ((subch & 7u) << 13) |
	    ((addr >> 2) & 0xFFFu);
}

/* ---- host methods ---- */

static inline size_t
uvm_host_set_object(uint32_t *w, uint32_t subch, uint32_t class_num)
{
	w[0] = uvm_method_header(UVM_METHOD_INCR, subch, UVM_HOST_SET_OBJECT, 1);
	w[1] = class_num;
	return (2);
}

/* a NOP header whose count skips `words` inline data words that follow it */
static inline size_t
uvm_host_nop_header(uint32_t *w, uint32_t words)
{
	w[0] = uvm_method_header(UVM_METHOD_NONINCR, UVM_SUBCH_HOST, UVM_HOST_NOP, words);
	return (1);
}

static inline size_t
uvm_host_wfi(uint32_t *w)
{
	w[0] = uvm_method_header(UVM_METHOD_INCR, UVM_SUBCH_HOST, UVM_HOST_WFI, 1);
	w[1] = 0;
	return (2);
}

#define	UVM_HOST_SEM_MAX_WORDS	6
#define	UVM_HOST_SEM_OP_ACQUIRE		0u
#define	UVM_HOST_SEM_OP_RELEASE		1u
#define	UVM_HOST_SEM_OP_ACQ_CIRC_GEQ	3u
#define	UVM_HOST_SEM_ACQUIRE_SWITCH_TSG	(1u << 12)
#define	UVM_HOST_SEM_RELEASE_WFI	(1u << 20)
#define	UVM_HOST_SEM_PAYLOAD_64BIT	(1u << 24)
#define	UVM_HOST_SEM_RELEASE_TIMESTAMP	(1u << 25)

static inline size_t
uvm_host_semaphore_group(uint32_t *w, uint64_t va, uint32_t payload, uint32_t execute)
{
	w[0] = uvm_method_header(UVM_METHOD_INCR, UVM_SUBCH_HOST, UVM_HOST_SEM_ADDR_LO, 5);
	w[1] = (uint32_t)va & ~3u;
	w[2] = (uint32_t)(va >> 32) & 0x1FFFFFFu;
	w[3] = payload;
	w[4] = 0;
	w[5] = execute;
	return (6);
}

/* release(va, payload): 32-bit payload, no WFI, no timestamp */
static inline size_t
uvm_host_semaphore_release(uint32_t *w, uint64_t va, uint32_t payload)
{
	return (uvm_host_semaphore_group(w, va, payload, UVM_HOST_SEM_OP_RELEASE));
}

/* acquire(va, payload): circular greater-or-equal, yield the TSG while waiting */
static inline size_t
uvm_host_semaphore_acquire(uint32_t *w, uint64_t va, uint32_t payload)
{
	return (uvm_host_semaphore_group(w, va, payload,
	    UVM_HOST_SEM_OP_ACQ_CIRC_GEQ | UVM_HOST_SEM_ACQUIRE_SWITCH_TSG));
}

#define	UVM_MEM_OP_MAX_WORDS	5

static inline size_t
uvm_host_mem_op(uint32_t *w, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	w[0] = uvm_method_header(UVM_METHOD_INCR, UVM_SUBCH_HOST, UVM_HOST_MEM_OP_A, 4);
	w[1] = a;
	w[2] = b;
	w[3] = c;
	w[4] = d;
	return (5);
}

/* membar system (type 0) or GPU (type 1) */
static inline size_t
uvm_host_membar(uint32_t *w, uvm_membar_t membar)
{
	return (uvm_host_mem_op(w, 0, 0, membar == UVM_MEMBAR_GPU ? 1u : 0u,
	    UVM_MEM_OP_MEMBAR << 27));
}

/* the "page table level" field: value = level base - UVM depth (base 6 for MMU v3, 5 for v2) */
static inline uint32_t
uvm_host_tlb_level(uint32_t depth, uint32_t mmu_version)
{
	uint32_t base = mmu_version == 3 ? 6u : 5u;

	return (depth >= base ? 0u : base - depth);
}

/* MEM_OP_C PDB fields: aperture 11:10, address bits 31:12 = (phys >> 12) low 20 bits */
static inline uint32_t
uvm_host_pdb_c(const struct uvm_pdb *pdb)
{
	uint32_t c = ((uint32_t)(pdb->phys >> 12) & 0xFFFFFu) << 12;

	c |= (pdb->vidmem ? UVM_HW_APERTURE_VID : UVM_HW_APERTURE_SYS_COHERENT) << 10;
	return (c);
}

/* MEM_OP_D PDB high bits 26:0 = (phys >> 12) >> 20 */
static inline uint32_t
uvm_host_pdb_d(const struct uvm_pdb *pdb)
{
	return ((uint32_t)((pdb->phys >> 12) >> 20) & 0x7FFFFFFu);
}

#define	UVM_MEM_OP_A_SCOPE_NON_LINK	(2u << 6)
#define	UVM_MEM_OP_A_SYSMEMBAR		(1u << 11)
#define	UVM_MEM_OP_C_PDB_ALL		(1u << 0)
#define	UVM_MEM_OP_C_GPC_DISABLE	(1u << 1)
#define	UVM_MEM_OP_C_ACK_GLOBALLY	(1u << 5)

/*
 * TLB invalidate all entries of one PDB down from `depth` (0 = root).  The
 * system membar is folded into the invalidate (sysmembar bit); any membar
 * asks for a global acknowledgement.  On Blackwell no standalone membar
 * follows (12 section 3).
 */
static inline size_t
uvm_host_tlb_invalidate_all(uint32_t *w, const struct uvm_pdb *pdb, uint32_t depth,
    uvm_membar_t membar, uint32_t mmu_version)
{
	uint32_t a = UVM_MEM_OP_A_SCOPE_NON_LINK;
	uint32_t c = uvm_host_pdb_c(pdb) | (uvm_host_tlb_level(depth, mmu_version) << 7);

	if (membar == UVM_MEMBAR_SYS)
		a |= UVM_MEM_OP_A_SYSMEMBAR;
	if (membar != UVM_MEMBAR_NONE)
		c |= UVM_MEM_OP_C_ACK_GLOBALLY;
	return (uvm_host_mem_op(w, a, 0, c, (UVM_MEM_OP_TLB_INVALIDATE << 27) | uvm_host_pdb_d(pdb)));
}

/*
 * uvm_tlb_invalidation_size - log2 of the number of 4K pages of the
 * smallest naturally aligned power-of-two block that covers [base,
 * base+size): the hardware aligns the target address down to the block,
 * so a block that covers the unaligned range is not enough on its own.
 * Returns 64 when no block below the whole address space covers it.
 * Time:  O(64).  Space: O(1).
 */
static inline uint32_t
uvm_tlb_invalidation_size(uint64_t base, uint64_t size)
{
	uint64_t first = base >> 12;
	uint64_t last = (base + size - 1) >> 12;	/* inclusive */
	uint32_t l;

	if (size == 0 || base + size - 1 < base)
		return (64);
	for (l = 0; l < 52; l++) {
		uint64_t block = 1ull << l;
		uint64_t start = first & ~(block - 1);

		if (start + block - 1 >= last)
			return (l);
	}
	return (64);
}

/* TLB invalidate the entries covering [base, base+size) (targeted, operation 10) */
static inline size_t
uvm_host_tlb_invalidate_va(uint32_t *w, const struct uvm_pdb *pdb, uint32_t depth, uint64_t base,
    uint64_t size, uvm_membar_t membar, uint32_t mmu_version)
{
	uint32_t l = uvm_tlb_invalidation_size(base, size);
	uint32_t a, c;

	if (l >= 52)
		return (uvm_host_tlb_invalidate_all(w, pdb, depth, membar, mmu_version));
	a = (l & 0x3Fu) | UVM_MEM_OP_A_SCOPE_NON_LINK | (((uint32_t)(base >> 12) & 0xFFFFFu) << 12);
	if (membar == UVM_MEMBAR_SYS)
		a |= UVM_MEM_OP_A_SYSMEMBAR;
	c = uvm_host_pdb_c(pdb) | (uvm_host_tlb_level(depth, mmu_version) << 7);
	if (membar != UVM_MEMBAR_NONE)
		c |= UVM_MEM_OP_C_ACK_GLOBALLY;
	return (uvm_host_mem_op(w, a, (uint32_t)(base >> 32), c,
	    (UVM_MEM_OP_TLB_INVALIDATE_TARGETED << 27) | uvm_host_pdb_d(pdb)));
}

/* replay faults: a targeted invalidate of VA 0 on a dummy PDB, PTE level only, replay field set */
static inline size_t
uvm_host_replay_faults(uint32_t *w, uvm_replay_t replay)
{
	uint32_t c = ((uint32_t)replay << 2) | (1u << 7) | (UVM_HW_APERTURE_VID << 10);

	return (uvm_host_mem_op(w, 0, 0, c, UVM_MEM_OP_TLB_INVALIDATE_TARGETED << 27));
}

/*
 * cancel the faults of one MMU engine at one 4K page: REPLAY field 5
 * (CANCEL_VA_GLOBAL), access type in the level field, the engine id in
 * MEM_OP_A (9 bits on Hopper and Blackwell, 7 on Volta through Ada).
 */
static inline size_t
uvm_host_cancel_faults_va(uint32_t *w, const struct uvm_pdb *pdb, uint64_t fault_va,
    uint32_t mmu_engine_id, uvm_cancel_access_t access, uint32_t engine_id_bits)
{
	uint32_t mask = (1u << engine_id_bits) - 1u;
	uint32_t a = (mmu_engine_id & mask) | (((uint32_t)(fault_va >> 12) & 0xFFFFFu) << 12);
	uint32_t c = uvm_host_pdb_c(pdb) | (5u << 2) | ((uint32_t)access << 7);

	return (uvm_host_mem_op(w, a, (uint32_t)(fault_va >> 32), c,
	    (UVM_MEM_OP_TLB_INVALIDATE_TARGETED << 27) | uvm_host_pdb_d(pdb)));
}

/* L2 invalidate of the system (or peer) aperture, bracketed by system membars */
#define	UVM_HOST_L2_INVALIDATE_MAX_WORDS	(3 * UVM_MEM_OP_MAX_WORDS)
static inline size_t
uvm_host_l2_invalidate(uint32_t *w, int peer)
{
	size_t n = 0;

	n += uvm_host_membar(w + n, UVM_MEMBAR_SYS);
	n += uvm_host_mem_op(w + n, 0, 0, 0,
	    (peer ? UVM_MEM_OP_L2_PEERMEM_INVALIDATE : UVM_MEM_OP_L2_SYSMEM_INVALIDATE) << 27);
	n += uvm_host_membar(w + n, UVM_MEMBAR_SYS);
	return (n);
}

/* clear-faulted software method: instance pointer and aperture, type (0 PBDMA, 1 engine) */
static inline size_t
uvm_sw_clear_faulted(uint32_t *w, uint64_t inst_phys, uvm_aperture_t inst_aperture, int engine_faulted)
{
	uint32_t a = ((uint32_t)(inst_phys >> 12) & 0xFFFFFu) << 12;

	a |= inst_aperture == UVM_APERTURE_VID ? UVM_HW_APERTURE_VID : UVM_HW_APERTURE_SYS_COHERENT;
	if (engine_faulted)
		a |= 1u << 2;
	w[0] = uvm_method_header(UVM_METHOD_INCR, UVM_SUBCH_SW, UVM_SW_CLEAR_FAULTED_A, 2);
	w[1] = a;
	w[2] = (uint32_t)(inst_phys >> 32);
	return (3);
}

/* ---- copy engine methods (subchannel 4) ---- */

#define	UVM_CE_LAUNCH_TRANSFER_NONE		0u
#define	UVM_CE_LAUNCH_TRANSFER_PIPELINED	1u
#define	UVM_CE_LAUNCH_TRANSFER_NON_PIPELINED	2u
#define	UVM_CE_LAUNCH_FLUSH_ENABLE		(1u << 2)
#define	UVM_CE_LAUNCH_SEM_RELEASE		(1u << 3)
#define	UVM_CE_LAUNCH_SEM_RELEASE_TIMESTAMP	(2u << 3)
#define	UVM_CE_LAUNCH_SRC_PITCH			(1u << 7)
#define	UVM_CE_LAUNCH_DST_PITCH			(1u << 8)
#define	UVM_CE_LAUNCH_REMAP_ENABLE		(1u << 10)
#define	UVM_CE_LAUNCH_SRC_PHYSICAL		(1u << 12)
#define	UVM_CE_LAUNCH_DST_PHYSICAL		(1u << 13)
#define	UVM_CE_LAUNCH_REDUCTION_INC		(6u << 14)
#define	UVM_CE_LAUNCH_REDUCTION_UNSIGNED	(1u << 18)
#define	UVM_CE_LAUNCH_REDUCTION_ENABLE		(1u << 19)
#define	UVM_CE_LAUNCH_MEMORY_SCRUB		(1u << 23)
#define	UVM_CE_LAUNCH_FLUSH_TYPE_GPU		(1u << 25)
#define	UVM_CE_LAUNCH_DISABLE_PLC		(1u << 26)

static inline uint32_t
uvm_ce_launch_flush(uvm_membar_t membar)
{
	switch (membar) {
	case UVM_MEMBAR_SYS:	return (UVM_CE_LAUNCH_FLUSH_ENABLE);
	case UVM_MEMBAR_GPU:	return (UVM_CE_LAUNCH_FLUSH_ENABLE | UVM_CE_LAUNCH_FLUSH_TYPE_GPU);
	default:		return (0);
	}
}

static inline size_t
uvm_ce_launch(uint32_t *w, uint32_t value)
{
	w[0] = uvm_method_header(UVM_METHOD_NONINCR, UVM_SUBCH_CE, UVM_CE_LAUNCH_DMA, 1);
	w[1] = value;
	return (2);
}

#define	UVM_CE_SEM_MAX_WORDS	6

static inline size_t
uvm_ce_semaphore_group(uint32_t *w, uint64_t va, uint32_t payload, uint32_t launch)
{
	w[0] = uvm_method_header(UVM_METHOD_INCR, UVM_SUBCH_CE, UVM_CE_SET_SEMAPHORE_A, 3);
	w[1] = (uint32_t)(va >> 32) & 0x1FFFFFFu;
	w[2] = (uint32_t)va;
	w[3] = payload;
	return (4 + uvm_ce_launch(w + 4, launch));
}

/* release(va, payload) with the push's membar as flush type; one-word payload */
static inline size_t
uvm_ce_semaphore_release(uint32_t *w, uint64_t va, uint32_t payload, uvm_membar_t membar)
{
	return (uvm_ce_semaphore_group(w, va, payload,
	    UVM_CE_LAUNCH_TRANSFER_NONE | uvm_ce_launch_flush(membar) | UVM_CE_LAUNCH_SEM_RELEASE |
	    UVM_CE_LAUNCH_DISABLE_PLC));
}

/* release with timestamp: the GPU writes payload plus an 8-byte timestamp (16 bytes) */
static inline size_t
uvm_ce_semaphore_timestamp(uint32_t *w, uint64_t va, uvm_membar_t membar)
{
	return (uvm_ce_semaphore_group(w, va, 0xdeadbeefu,
	    UVM_CE_LAUNCH_TRANSFER_NONE | uvm_ce_launch_flush(membar) |
	    UVM_CE_LAUNCH_SEM_RELEASE_TIMESTAMP | UVM_CE_LAUNCH_DISABLE_PLC));
}

/* SET_*_PHYS_MODE data: target 1:0 (0 local FB, 1 coherent sysmem, 3 peer), peer id 8:6 */
static inline uint32_t
uvm_ce_phys_mode(uvm_aperture_t aperture)
{
	if (aperture == UVM_APERTURE_VID)
		return (0);
	if (aperture == UVM_APERTURE_SYS)
		return (1);
	return (3u | (UVM_APERTURE_PEER_ID(aperture) << 6));
}

/* the pieces of a copy or memset: offsets group, line length, launch */
static inline size_t
uvm_ce_piece_x(uint32_t *w, uint64_t dst, const uint64_t *src, uint32_t length, uint32_t launch, int line_count)
{
	size_t n = 0;

	if (src != NULL) {
		w[n++] = uvm_method_header(UVM_METHOD_INCR, UVM_SUBCH_CE, UVM_CE_OFFSET_IN_UPPER, 4);
		w[n++] = (uint32_t)(*src >> 32) & 0x1FFFFFFu;
		w[n++] = (uint32_t)*src;
	} else {
		w[n++] = uvm_method_header(UVM_METHOD_INCR, UVM_SUBCH_CE, UVM_CE_OFFSET_OUT_UPPER, 2);
	}
	w[n++] = (uint32_t)(dst >> 32) & 0x1FFFFFFu;
	w[n++] = (uint32_t)dst;
	w[n++] = uvm_method_header(UVM_METHOD_INCR, UVM_SUBCH_CE, UVM_CE_LINE_LENGTH_IN, 1);
	w[n++] = length;
	if (line_count) {	/* probe only: LINE_COUNT 0x41C = 1 line (clca0b5.h) */
		w[n++] = uvm_method_header(UVM_METHOD_INCR, UVM_SUBCH_CE, UVM_CE_LINE_COUNT, 1);
		w[n++] = 1;
	}
	n += uvm_ce_launch(w + n, launch);
	return (n);
}

static inline size_t
uvm_ce_piece(uint32_t *w, uint64_t dst, const uint64_t *src, uint32_t length, uint32_t launch)
{
	return (uvm_ce_piece_x(w, dst, src, length, launch, 0));
}

/* words one piece needs (phys-mode group + offsets + length + launch) */
#define	UVM_CE_PIECE_MAX_WORDS	(3 + 5 + 2 + 2 + 2)
#define	UVM_CE_MEMSET_HEADER_WORDS	4

/*
 * memcopy(dst, src, size): physical addresses get a SET_*_PHYS_MODE first
 * (both in one 2-word group when both are physical); the transfer is split
 * in pieces of at most UVM_CE_MAX_PIECE bytes; the flush goes on the last
 * piece; the first piece is non-pipelined unless `pipelined`.  Returns the
 * words written; the caller sizes the buffer from uvm_ce_memcopy_words().
 */
static inline size_t
uvm_ce_memcopy_pieces(uint64_t size)
{
	return (size == 0 ? 1 : (size_t)((size + UVM_CE_MAX_PIECE - 1) / UVM_CE_MAX_PIECE));
}

static inline size_t
uvm_ce_memcopy_words(uint64_t size)
{
	return (3 + uvm_ce_memcopy_pieces(size) * UVM_CE_PIECE_MAX_WORDS);
}

static inline size_t
uvm_ce_memcopy_x(uint32_t *w, const struct uvm_gpu_address *dst, const struct uvm_gpu_address *src,
    uint64_t size, uvm_membar_t membar, int pipelined, int line_count)
{
	size_t n = 0;
	uint64_t done = 0;
	uint32_t base = UVM_CE_LAUNCH_SRC_PITCH | UVM_CE_LAUNCH_DST_PITCH | UVM_CE_LAUNCH_DISABLE_PLC;

	if (!src->is_virtual && !dst->is_virtual) {
		w[n++] = uvm_method_header(UVM_METHOD_INCR, UVM_SUBCH_CE, UVM_CE_SET_SRC_PHYS_MODE, 2);
		w[n++] = uvm_ce_phys_mode(src->aperture);
		w[n++] = uvm_ce_phys_mode(dst->aperture);
	} else if (!src->is_virtual) {
		w[n++] = uvm_method_header(UVM_METHOD_INCR, UVM_SUBCH_CE, UVM_CE_SET_SRC_PHYS_MODE, 1);
		w[n++] = uvm_ce_phys_mode(src->aperture);
	} else if (!dst->is_virtual) {
		w[n++] = uvm_method_header(UVM_METHOD_INCR, UVM_SUBCH_CE, UVM_CE_SET_DST_PHYS_MODE, 1);
		w[n++] = uvm_ce_phys_mode(dst->aperture);
	}
	if (!src->is_virtual)
		base |= UVM_CE_LAUNCH_SRC_PHYSICAL;
	if (!dst->is_virtual)
		base |= UVM_CE_LAUNCH_DST_PHYSICAL;

	do {
		uint64_t piece = size - done;
		uint64_t s = src->address + done;
		uint32_t launch = base;

		if (piece > UVM_CE_MAX_PIECE)
			piece = UVM_CE_MAX_PIECE;
		launch |= (done == 0 && !pipelined) ? UVM_CE_LAUNCH_TRANSFER_NON_PIPELINED :
		    UVM_CE_LAUNCH_TRANSFER_PIPELINED;
		if (done + piece >= size)
			launch |= uvm_ce_launch_flush(membar);
		n += uvm_ce_piece_x(w + n, dst->address + done, &s, (uint32_t)piece, launch, line_count);
		done += piece;
	} while (done < size);
	return (n);
}

static inline size_t
uvm_ce_memcopy(uint32_t *w, const struct uvm_gpu_address *dst, const struct uvm_gpu_address *src,
    uint64_t size, uvm_membar_t membar, int pipelined)
{
	return (uvm_ce_memcopy_x(w, dst, src, size, membar, pipelined, 0));
}

#define	UVM_CE_MEMSET_MAX_WORDS(size)	(UVM_CE_MEMSET_HEADER_WORDS + 2 + \
	uvm_ce_memcopy_pieces(size) * UVM_CE_PIECE_MAX_WORDS)

/*
 * memset with 8-byte elements: constants A (low word) and B (high word) to
 * components X and Y, 4-byte components, two of them; LINE_LENGTH_IN counts
 * elements.
 */
/* probe-only memset variants (hw.nvidia_uvm.init_probe_work); defaults use none */
#define	UVM_CE_MEMSET_X_BLOCKLINEAR	0x1u	/* probe only: source layout block-linear (raised Xid 71) */
#define	UVM_CE_MEMSET_X_EXPLICIT_REMAP	0x2u	/* remap DST_Z and DST_W NO_WRITE */
#define	UVM_CE_MEMSET_X_LINE_COUNT	0x4u	/* LINE_COUNT = 1 before the launch */

static inline size_t
uvm_ce_memset_8_x(uint32_t *w, const struct uvm_gpu_address *dst, uint64_t value, uint64_t size,
    uvm_membar_t membar, unsigned xflags)
{
	size_t n = 0;
	uint64_t done = 0;
	uint32_t base = UVM_CE_LAUNCH_DST_PITCH | UVM_CE_LAUNCH_REMAP_ENABLE | UVM_CE_LAUNCH_DISABLE_PLC |
	    UVM_CE_LAUNCH_TRANSFER_NON_PIPELINED;

	if (!dst->is_virtual) {
		w[n++] = uvm_method_header(UVM_METHOD_INCR, UVM_SUBCH_CE, UVM_CE_SET_DST_PHYS_MODE, 1);
		w[n++] = uvm_ce_phys_mode(dst->aperture);
		base |= UVM_CE_LAUNCH_DST_PHYSICAL;
	}
	w[n++] = uvm_method_header(UVM_METHOD_INCR, UVM_SUBCH_CE, UVM_CE_SET_REMAP_CONST_A, 3);
	w[n++] = (uint32_t)value;
	w[n++] = (uint32_t)(value >> 32);
	/*
	 * Source layout PITCH on every memset (test boot 5, Q-010): left at the
	 * hardware default, block-linear, the engine raised Xid 71 even with a
	 * virtual destination; with PITCH set the same memset completes.
	 */
	if (!(xflags & UVM_CE_MEMSET_X_BLOCKLINEAR))
		base |= UVM_CE_LAUNCH_SRC_PITCH;
	/* X from const A, Y from const B, 4-byte components, two destination components */
	w[n++] = 4u | (5u << 4) | (3u << 16) | (1u << 24);
	if (xflags & UVM_CE_MEMSET_X_EXPLICIT_REMAP)	/* Z and W NO_WRITE (6), one source component */
		w[n - 1] |= (6u << 8) | (6u << 12);
	do {
		uint64_t piece = size - done;
		uint32_t launch = base;

		if (piece > UVM_CE_MAX_PIECE)
			piece = UVM_CE_MAX_PIECE;
		if (done + piece >= size)
			launch |= uvm_ce_launch_flush(membar);
		n += uvm_ce_piece_x(w + n, dst->address + done, NULL, (uint32_t)(piece / 8), launch,
		    (xflags & UVM_CE_MEMSET_X_LINE_COUNT) != 0);
		done += piece;
	} while (done < size);
	return (n);
}

static inline size_t
uvm_ce_memset_8(uint32_t *w, const struct uvm_gpu_address *dst, uint64_t value, uint64_t size,
    uvm_membar_t membar)
{
	return (uvm_ce_memset_8_x(w, dst, value, size, membar, 0));
}

/* ---- 1.2 GPFIFO entries ---- */

/* a normal entry: VA bits 31:2 and 39:32, length in words, no sync, main level */
static inline uint64_t
uvm_gpfifo_entry(uint64_t pb_va, uint32_t length_words)
{
	uint32_t lo = (uint32_t)pb_va & ~3u;
	uint32_t hi = ((uint32_t)(pb_va >> 32) & 0xFFu) | ((length_words & 0x1FFFFFu) << 10);

	return ((uint64_t)hi << 32 | lo);
}

/* the control entry that sets a channel's pushbuffer segment base: VA bits 56:40 */
static inline uint64_t
uvm_gpfifo_segment_base_entry(uint64_t pb_va)
{
	uint32_t lo = ((uint32_t)(pb_va >> 40) & 0x1FFFFu) << 8;
	uint32_t hi = 4u;

	return ((uint64_t)hi << 32 | lo);
}

/* ---- 12 section 5: fault buffer entries (class 0xC369, 32 bytes) ---- */

#define	UVM_FAULT_ENTRY_BYTES	32
#define	UVM_FAULT_ENTRY_WORDS	8

typedef enum { UVM_FAULT_CLIENT_GPC = 0, UVM_FAULT_CLIENT_HUB = 1 } uvm_fault_client_type_t;

struct uvm_fault_entry {
	uint64_t	inst_phys;
	uint64_t	fault_address;		/* raw, before canonicalisation */
	uint64_t	timestamp;
	uvm_aperture_t	inst_aperture;
	uint32_t	mmu_engine_id;
	uint8_t		fault_type;
	uint8_t		client_id;
	uint8_t		access_type;
	uint8_t		gpc_id;
	uvm_fault_client_type_t client_type;
	uint8_t		replayable;
	uint8_t		valid;
	uint8_t		protected_mode;
};

static inline uvm_aperture_t
uvm_hw_aperture_to_aperture(uint32_t hw)
{
	return (hw == UVM_HW_APERTURE_VID ? UVM_APERTURE_VID : UVM_APERTURE_SYS);
}

static inline void
uvm_fault_entry_decode(const uint32_t *e, struct uvm_fault_entry *f)
{
	f->inst_aperture = uvm_hw_aperture_to_aperture((e[0] >> 8) & 3u);
	f->inst_phys = ((uint64_t)e[1] << 32) | ((uint64_t)(e[0] >> 12) << 12);
	f->fault_address = ((uint64_t)e[3] << 32) | ((uint64_t)(e[2] >> 12) << 12);
	f->timestamp = ((uint64_t)e[5] << 32) | e[4];
	f->mmu_engine_id = e[6] & 0x1FFu;
	f->fault_type = (uint8_t)(e[7] & 0x1Fu);
	f->replayable = (uint8_t)((e[7] >> 7) & 1u);
	f->client_id = (uint8_t)((e[7] >> 8) & 0x7Fu);
	f->access_type = (uint8_t)((e[7] >> 16) & 0xFu);
	f->client_type = ((e[7] >> 20) & 1u) ? UVM_FAULT_CLIENT_HUB : UVM_FAULT_CLIENT_GPC;
	f->gpc_id = (uint8_t)((e[7] >> 24) & 0x1Fu);
	f->protected_mode = (uint8_t)((e[7] >> 29) & 1u);
	f->valid = (uint8_t)((e[7] >> 31) & 1u);
}

/* the entry's valid bit (word 7 bit 31), for the fetch loop and for clearing it */
#define	UVM_FAULT_ENTRY_VALID_WORD	7
#define	UVM_FAULT_ENTRY_VALID_BIT	(1u << 31)

/* replayable fault GET/PUT registers: PTR 19:0; GET has GETPTR_CORRUPTED 30 and OVERFLOW 31 */
static inline uint32_t
uvm_fault_buffer_get_ptr(uint32_t reg)
{
	return (reg & 0xFFFFFu);
}

static inline int
uvm_fault_buffer_get_overflow(uint32_t reg)
{
	return ((reg >> 31) & 1u);
}

static inline int
uvm_fault_buffer_get_corrupted(uint32_t reg)
{
	return ((reg >> 30) & 1u);
}

/* the GET write: the index plus the two write-one-to-clear status bits */
static inline uint32_t
uvm_fault_buffer_get_write(uint32_t index)
{
	return ((index & 0xFFFFFu) | (1u << 30) | (1u << 31));
}

/* sign-extend a GPU address to the CPU's canonical form of `width` bits */
static inline uint64_t
uvm_canonical_address(uint64_t addr, unsigned width)
{
	uint64_t sign = 1ull << (width - 1);

	addr &= (sign << 1) - 1;
	return ((addr ^ sign) - sign);
}

/* fault types (12 section 5 / 01 section 7.5 numbering is the tools one; these are the hardware codes) */
#define	UVM_HW_FAULT_TYPE_PDE			0u
#define	UVM_HW_FAULT_TYPE_PDE_SIZE		1u
#define	UVM_HW_FAULT_TYPE_PTE			2u
#define	UVM_HW_FAULT_TYPE_VA_LIMIT		3u
#define	UVM_HW_FAULT_TYPE_UNBOUND_INST		4u
#define	UVM_HW_FAULT_TYPE_PRIV			5u
#define	UVM_HW_FAULT_TYPE_RO			6u
#define	UVM_HW_FAULT_TYPE_WO			7u
#define	UVM_HW_FAULT_TYPE_PITCH_MASK		8u
#define	UVM_HW_FAULT_TYPE_WORK_CREATION		9u
#define	UVM_HW_FAULT_TYPE_UNSUPPORTED_APERTURE	10u
#define	UVM_HW_FAULT_TYPE_CC_VIOLATION		11u
#define	UVM_HW_FAULT_TYPE_UNSUPPORTED_KIND	12u
#define	UVM_HW_FAULT_TYPE_REGION_VIOLATION	13u
#define	UVM_HW_FAULT_TYPE_POISONED		14u
#define	UVM_HW_FAULT_TYPE_ATOMIC		15u

/* ---- 12 section 1: generations ---- */

typedef enum {
	UVM_MMU_ENGINE_UNKNOWN = 0,
	UVM_MMU_ENGINE_GRAPHICS,
	UVM_MMU_ENGINE_CE,
	UVM_MMU_ENGINE_HOST,
	UVM_MMU_ENGINE_BAR1,
	UVM_MMU_ENGINE_BAR2
} uvm_mmu_engine_type_t;

struct uvm_hal_generation {
	const char	*name;
	uint32_t	arch;			/* RM architecture id */
	uint32_t	host_class;
	uint32_t	host_class_alt;		/* a second class RM may report, or 0 */
	uint32_t	ce_class;
	uint32_t	ce_class_alt;
	uint32_t	mmu_version;		/* 2 or 3 */
	uint64_t	max_channel_va;		/* 2^40 or 2^57 */
	uint64_t	rm_va_size;		/* internal address space: 128TB or 64PB */
	uint32_t	cancel_engine_id_bits;	/* 7 or 9 */
	uint8_t		needs_pb_segment_base;
	uint8_t		supported;		/* this implementation has its HAL */
	/* MMU engine id ranges (inclusive) */
	uint32_t	ce_id_first, ce_id_last;
	uint32_t	host_id_first, host_id_last;
	uint32_t	graphics_id_base;
	uint32_t	bar1_id, bar2_id;
};

#define	UVM_ARCH_AMPERE		0x170u
#define	UVM_ARCH_HOPPER		0x180u
#define	UVM_ARCH_ADA		0x190u
#define	UVM_ARCH_BLACKWELL_GB100 0x1A0u
#define	UVM_ARCH_BLACKWELL_GB20X 0x1B0u

static const struct uvm_hal_generation uvm_hal_generations[] = {
	{ "Ampere", UVM_ARCH_AMPERE, 0xC56F, 0, 0xC6B5, 0xC7B5, 2, 1ull << 40, 1ull << 47, 7, 0, 0,
	    15, 24, 32, 63, 64, 128, 192 },
	{ "Ada", UVM_ARCH_ADA, 0xC56F, 0, 0xC7B5, 0, 2, 1ull << 40, 1ull << 47, 7, 0, 0,
	    15, 24, 32, 63, 64, 128, 192 },
	{ "Hopper", UVM_ARCH_HOPPER, 0xC86F, 0, 0xC8B5, 0, 3, 1ull << 57, 1ull << 56, 9, 1, 0,
	    43, 52, 64, 108, 384, 256, 320 },
	{ "Blackwell GB100", UVM_ARCH_BLACKWELL_GB100, 0xC96F, 0, 0xC9B5, 0, 3, 1ull << 57, 1ull << 56,
	    9, 1, 0, 65, 84, 85, 129, 384, 256, 320 },
	{ "Blackwell GB20x", UVM_ARCH_BLACKWELL_GB20X, 0xCA6F, 0xC96F, 0xCAB5, 0xC9B5, 3, 1ull << 57,
	    1ull << 56, 9, 1, 1, 65, 84, 85, 129, 384, 256, 320 },
};

static inline const struct uvm_hal_generation *
uvm_hal_generation_by_arch(uint32_t arch)
{
	size_t i;

	for (i = 0; i < sizeof(uvm_hal_generations) / sizeof(uvm_hal_generations[0]); i++)
		if (uvm_hal_generations[i].arch == arch)
			return (&uvm_hal_generations[i]);
	return (NULL);
}

/* does this implementation drive the (arch, host class, CE class) RM reported? */
static inline int
uvm_hal_classes_supported(uint32_t arch, uint32_t host_class, uint32_t ce_class)
{
	const struct uvm_hal_generation *g = uvm_hal_generation_by_arch(arch);

	if (g == NULL || !g->supported)
		return (0);
	if (host_class != g->host_class && (g->host_class_alt == 0 || host_class != g->host_class_alt))
		return (0);
	if (ce_class != g->ce_class && (g->ce_class_alt == 0 || ce_class != g->ce_class_alt))
		return (0);
	return (1);
}

static inline uvm_mmu_engine_type_t
uvm_hal_engine_type(const struct uvm_hal_generation *g, uint32_t id)
{
	if (id >= g->ce_id_first && id <= g->ce_id_last)
		return (UVM_MMU_ENGINE_CE);
	if (id >= g->host_id_first && id <= g->host_id_last)
		return (UVM_MMU_ENGINE_HOST);
	if (id == g->bar1_id)
		return (UVM_MMU_ENGINE_BAR1);
	if (id == g->bar2_id)
		return (UVM_MMU_ENGINE_BAR2);
	if (id >= g->graphics_id_base)
		return (UVM_MMU_ENGINE_GRAPHICS);
	return (UVM_MMU_ENGINE_UNKNOWN);
}

static inline uvm_mmu_engine_type_t
uvm_hal_blackwell_engine_type(uint32_t id)
{
	return (uvm_hal_engine_type(uvm_hal_generation_by_arch(UVM_ARCH_BLACKWELL_GB20X), id));
}

#endif /* _UVM_HAL_METHODS_H_ */
