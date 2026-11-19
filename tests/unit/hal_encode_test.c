/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * hal_encode_test.c - the pure method, GPFIFO-entry and fault-entry
 * encoders of sys/uvm_hal_methods.h produce the bit patterns of uvm-spec r1,
 * 04-channels-and-copy-engine.md section 1 and 12-hal-per-generation.md
 * sections 1, 5 and 7, for the Blackwell GB20x host class (0xCA6F) and
 * copy class (0xCAB5), and the generation tables answer as section 12 says.
 *
 * Red first: compiled before the header existed.
 */

#include <stdio.h>
#include <string.h>

#include "uvm_hal_methods.h"

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
	uint32_t w[32];
	size_t n;

	/* 1.1 method headers: opcode 31:29, count 28:16, subchannel 15:13, addr/4 11:0 */
	CHECK(uvm_method_header(UVM_METHOD_INCR, 0, 0x0028, 4) == 0x2004000a,
	    "incrementing header %#x", uvm_method_header(UVM_METHOD_INCR, 0, 0x0028, 4));
	CHECK(uvm_method_header(UVM_METHOD_NONINCR, 4, 0x0300, 1) == 0x600180c0,
	    "non-incrementing header on subchannel 4");
	CHECK(uvm_method_header(UVM_METHOD_NONINCR, 0, 0x0008, 8191) == 0x7fff0002,
	    "NOP header carrying the maximum inline count");

	/* SET_OBJECT on subchannel 0 and 5 */
	n = uvm_host_set_object(w, 0, 0xCAB5);
	CHECK(n == 2 && w[0] == 0x20010000 && w[1] == 0xCAB5, "SET_OBJECT subch 0");
	n = uvm_host_set_object(w, 5, 0xC076);
	CHECK(n == 2 && w[0] == 0x2001a000 && w[1] == 0xC076, "SET_OBJECT subch 5");

	/* 1.3 host semaphore release: 5-word group at 0x5C; op release, 32-bit, no WFI, no ts */
	n = uvm_host_semaphore_release(w, 0x1234567890ULL, 0xabcd);
	CHECK(n == 6 && w[0] == 0x20050017 && w[1] == 0x34567890 && w[2] == 0x12 &&
	    w[3] == 0xabcd && w[4] == 0 && w[5] == 0x1, "host semaphore release: %#x %#x %#x %#x %#x %#x",
	    w[0], w[1], w[2], w[3], w[4], w[5]);
	/* acquire: op 3 (circular >=), switch TSG bit 12 */
	n = uvm_host_semaphore_acquire(w, 0x1000, 7);
	CHECK(n == 6 && w[3] == 7 && w[5] == ((1u << 12) | 3u), "host semaphore acquire execute %#x", w[5]);

	/* WFI, NOP */
	n = uvm_host_wfi(w);
	CHECK(n == 2 && w[0] == 0x2001001e && w[1] == 0, "WFI");
	n = uvm_host_nop_header(w, 100);
	CHECK(n == 1 && w[0] == 0x60640002, "NOP header skipping 100 words: %#x", w[0]);

	/* membar system / GPU: MEM_OP_D operation 5 << 27, MEM_OP_C bits 2:0 type */
	n = uvm_host_membar(w, UVM_MEMBAR_SYS);
	CHECK(n == 5 && w[0] == 0x2004000a && w[3] == 0 && w[4] == (5u << 27), "membar sys");
	n = uvm_host_membar(w, UVM_MEMBAR_GPU);
	CHECK(n == 5 && w[3] == 1 && w[4] == (5u << 27), "membar gpu");

	/* TLB invalidate all: PDB one, GPC enable, level = 6 - depth, aperture, ack globally, sysmembar */
	{
		struct uvm_pdb pdb = { .phys = 0x123456789000ULL, .vidmem = 0 };

		n = uvm_host_tlb_invalidate_all(w, &pdb, 0, UVM_MEMBAR_SYS, 3);
		CHECK(n == 5, "tlb inval all words %zu", n);
		/* A: sysmembar bit 11, scope non-link (2 << 6) */
		CHECK(w[1] == ((1u << 11) | (2u << 6)), "MEM_OP_A %#x", w[1]);
		CHECK(w[2] == 0, "MEM_OP_B %#x", w[2]);
		/* C: pdb one (0), gpc enable (0), replay none, ack GLOBALLY (1<<5), level 6-0=6 at bits 9:7,
		 * aperture coherent sysmem (2 << 10), pdb addr lo: (phys>>12) low 20 bits << 12 */
		CHECK(w[3] == ((1u << 5) | (6u << 7) | (2u << 10) | (0x56789u << 12)),
		    "MEM_OP_C %#x", w[3]);
		/* D: operation 9 << 27 | pdb hi = (phys>>12)>>20 */
		CHECK(w[4] == ((9u << 27) | 0x1234u), "MEM_OP_D %#x", w[4]);

		/* GPU membar: no sysmembar bit, still ack globally; vidmem aperture 0 */
		pdb.vidmem = 1;
		n = uvm_host_tlb_invalidate_all(w, &pdb, 4, UVM_MEMBAR_GPU, 3);
		CHECK(w[1] == (2u << 6) && (w[3] & (3u << 10)) == 0 && ((w[3] >> 7) & 7) == 2,
		    "tlb inval all gpu: A %#x C %#x", w[1], w[3]);
		/* no membar: ack none */
		n = uvm_host_tlb_invalidate_all(w, &pdb, 5, UVM_MEMBAR_NONE, 3);
		CHECK((w[3] & (3u << 5)) == 0 && ((w[3] >> 7) & 7) == 1, "tlb inval all none");
	}

	/* TLB invalidate VA: operation 10, size field = log2(pages) covering the range */
	{
		struct uvm_pdb pdb = { .phys = 0x1000, .vidmem = 1 };

		n = uvm_host_tlb_invalidate_va(w, &pdb, 5, 0xABCD0000ULL, 0x3000, UVM_MEMBAR_NONE, 3);
		CHECK(n == 5 && (w[4] >> 27) == 10, "targeted invalidate op");
		CHECK((w[1] & 0x3f) == 2, "invalidation size log2(3 pages -> 4) = 2, got %u", w[1] & 0x3f);
		CHECK((w[1] >> 12) == 0xABCD0, "target VA lo %#x", w[1] >> 12);
		/* the hardware aligns the target down to the block: an unaligned range needs a
		 * block that still covers its end (pages 0xABCDE..0xABCE0 need a 64-page block) */
		n = uvm_host_tlb_invalidate_va(w, &pdb, 5, 0xABCDE000ULL, 0x3000, UVM_MEMBAR_NONE, 3);
		CHECK((w[1] & 0x3f) == 6, "aligned-down coverage: want 6, got %u", w[1] & 0x3f);
		CHECK(uvm_tlb_invalidation_size(0, 0x1000) == 0 && uvm_tlb_invalidation_size(0x1000, 0x1000) == 0 &&
		    uvm_tlb_invalidation_size(0x1000, 0x2000) == 2, "size helper edges");
		CHECK(w[2] == 0, "target VA hi");
		/* whole space: falls back to invalidate all */
		n = uvm_host_tlb_invalidate_va(w, &pdb, 5, 0, ~0ULL, UVM_MEMBAR_NONE, 3);
		CHECK((w[4] >> 27) == 9, "whole-space range becomes invalidate all");
	}

	/* replay START: targeted, VA 0, PDB 0, level PTE only (1), aperture vid, replay 1 */
	n = uvm_host_replay_faults(w, UVM_REPLAY_START);
	CHECK(n == 5 && (w[4] >> 27) == 10 && ((w[3] >> 2) & 7) == 1 && ((w[3] >> 7) & 7) == 1,
	    "replay start C %#x D %#x", w[3], w[4]);
	n = uvm_host_replay_faults(w, UVM_REPLAY_START_ACK_ALL);
	CHECK(((w[3] >> 2) & 7) == 2, "replay start ack all");

	/* cancel by VA: replay 5, access type 7 at bits 9:7, engine id in A bits 8:0 (Hopper+) */
	{
		struct uvm_pdb pdb = { .phys = 0x5000, .vidmem = 0 };

		n = uvm_host_cancel_faults_va(w, &pdb, 0x7F00000000ULL, 0x1FF, UVM_CANCEL_ACCESS_ALL, 9);
		CHECK(n == 5 && (w[1] & 0x1ff) == 0x1FF, "cancel engine id %#x", w[1] & 0x1ff);
		CHECK((w[1] >> 12) == 0, "cancel VA lo bits for 0x7F00000000: %#x", w[1] >> 12);
		CHECK(w[2] == 0x7F, "cancel VA hi %#x", w[2]);
		CHECK(((w[3] >> 2) & 7) == 5 && ((w[3] >> 7) & 7) == 7 && ((w[3] >> 10) & 3) == 2,
		    "cancel C %#x", w[3]);
		n = uvm_host_cancel_faults_va(w, &pdb, 0, 1, UVM_CANCEL_ACCESS_WRITE_ATOMIC, 7);
		CHECK(((w[3] >> 7) & 7) == 6 && (w[1] & 0x1ff) == 1 && (w[1] & 0x180) == 0,
		    "cancel write+atomic, 7-bit engine field");
	}

	/* 1.4 CE semaphore release: SET_SEMAPHORE_A/B/PAYLOAD + LAUNCH_DMA */
	n = uvm_ce_semaphore_release(w, 0x00ABCDEF12345678ULL, 0x55, UVM_MEMBAR_SYS);
	CHECK(n == 6, "ce sem release words %zu", n);
	CHECK(w[0] == uvm_method_header(UVM_METHOD_INCR, 4, 0x0240, 3) && w[1] == 0xABCDEF &&
	    w[2] == 0x12345678 && w[3] == 0x55, "ce sem address/payload %#x %#x %#x", w[1], w[2], w[3]);
	CHECK(w[4] == uvm_method_header(UVM_METHOD_NONINCR, 4, 0x0300, 1), "launch header");
	/* transfer none(0), flush enable bit 2, sem type 1 << 3, disable PLC bit 26, flush type sys(0) */
	CHECK(w[5] == ((1u << 2) | (1u << 3) | (1u << 26)), "launch dma %#x", w[5]);
	n = uvm_ce_semaphore_release(w, 0x1000, 1, UVM_MEMBAR_GPU);
	CHECK(w[5] == ((1u << 2) | (1u << 3) | (1u << 25) | (1u << 26)), "launch dma gpu flush %#x", w[5]);
	n = uvm_ce_semaphore_release(w, 0x1000, 1, UVM_MEMBAR_NONE);
	CHECK(w[5] == ((1u << 3) | (1u << 26)), "launch dma no flush %#x", w[5]);
	n = uvm_ce_semaphore_timestamp(w, 0x2000, UVM_MEMBAR_SYS);
	CHECK(((w[5] >> 3) & 3) == 2, "timestamp semaphore type");

	/* memcopy virtual -> physical sysmem, one piece, pipelined last */
	{
		struct uvm_gpu_address dst = { .address = 0x80000000ULL, .is_virtual = 0,
		    .aperture = UVM_APERTURE_SYS };
		struct uvm_gpu_address src = { .address = 0x7000ULL, .is_virtual = 1 };

		n = uvm_ce_memcopy(w, &dst, &src, 0x1000, UVM_MEMBAR_SYS, 0);
		/* SET_DST_PHYS_MODE target coherent sysmem (1) */
		CHECK(w[0] == uvm_method_header(UVM_METHOD_INCR, 4, 0x0264, 1) && w[1] == 1,
		    "dst phys mode %#x %#x", w[0], w[1]);
		CHECK(w[2] == uvm_method_header(UVM_METHOD_INCR, 4, 0x0400, 4) && w[3] == 0 &&
		    w[4] == 0x7000 && w[5] == 0 && w[6] == 0x80000000u, "offsets");
		CHECK(w[7] == uvm_method_header(UVM_METHOD_INCR, 4, 0x0418, 1) && w[8] == 0x1000, "line length");
		/* non-pipelined (2), flush en, pitch src (bit 7) dst (bit 8), dst phys (bit 13), disable PLC */
		CHECK(w[10] == (2u | (1u << 2) | (1u << 7) | (1u << 8) | (1u << 13) | (1u << 26)),
		    "memcopy launch %#x", w[10]);
		CHECK(n == 11, "memcopy words %zu", n);
		/* vidmem physical destination target 0, peer target 3 with peer id bits 8:6 */
		dst.aperture = UVM_APERTURE_VID;
		n = uvm_ce_memcopy(w, &dst, &src, 0x1000, UVM_MEMBAR_NONE, 1);
		CHECK(w[1] == 0 && (w[10] & 3) == 1 && (w[10] & (1u << 2)) == 0, "vid dst, pipelined, no flush");
		dst.aperture = UVM_APERTURE_PEER0 + 2;
		n = uvm_ce_memcopy(w, &dst, &src, 0x1000, UVM_MEMBAR_NONE, 1);
		CHECK(w[1] == (3u | (2u << 6)), "peer 2 phys mode %#x", w[1]);
	}

	/* memset 8 bytes: remap A->X, B->Y, size 4, two components, remap enable bit 10 */
	{
		struct uvm_gpu_address dst = { .address = 0x9000, .is_virtual = 1 };

		n = uvm_ce_memset_8(w, &dst, 0x1122334455667788ULL, 0x100, UVM_MEMBAR_SYS);
		CHECK(w[0] == uvm_method_header(UVM_METHOD_INCR, 4, 0x0700, 3) && w[1] == 0x55667788 &&
		    w[2] == 0x11223344, "remap constants %#x %#x", w[1], w[2]);
		CHECK(w[3] == (4u | (5u << 4) | (3u << 16) | (1u << 24)), "remap components %#x", w[3]);
		/* element count = bytes / 8 */
		CHECK(w[8] == 0x20, "element count %#x", w[8]);
		CHECK((w[10] & (1u << 10)) && (w[10] & (1u << 13)) == 0, "memset launch %#x", w[10]);
	}

	/* 1.2 GPFIFO entries */
	{
		uint64_t e = uvm_gpfifo_entry(0x00123456789ABC00ULL, 24);

		CHECK((uint32_t)e == ((0x789ABC00u >> 2) << 2), "entry lo %#x", (uint32_t)e);
		CHECK((e >> 32) == (0x56u | (24u << 10)), "entry hi %#x", (uint32_t)(e >> 32));
		e = uvm_gpfifo_segment_base_entry(0x00123456789ABC00ULL);
		/* lo: opcode field bits 7:0 = 4? no: control entry: length 0, high word opcode bits 7:0 = 4,
		 * low word bits 24:8 carry VA bits 56:40 */
		CHECK((e >> 32) == 4 && (uint32_t)e == ((0x001234u) << 8), "segment base entry %#llx",
		    (unsigned long long)e);
	}

	/* 12 section 5: fault entry decode (class 0xC369) */
	{
		uint32_t entry[8] = {
			(0x2u << 8) | (0xABCDEu << 12),	/* inst aperture sysmem coherent, inst lo */
			0x00000012,			/* inst hi */
			(0x7F000u << 12),		/* addr lo (bits 31:12 of VA) */
			0x00007FFF,			/* addr hi: VA = 0x7FFF7F000000 */
			0xdeadbeef, 0x00000001,		/* timestamp */
			0x45,				/* engine id 69 -> Blackwell CE4 */
			(2u << 0) | (1u << 7) | (0x18u << 8) | (2u << 16) | (1u << 20) | (3u << 24) | (1u << 31)
		};
		struct uvm_fault_entry f;

		uvm_fault_entry_decode(entry, &f);
		CHECK(f.valid && f.replayable, "valid/replayable");
		CHECK(f.inst_aperture == UVM_APERTURE_SYS && f.inst_phys == 0x12ABCDE000ULL,
		    "instance pointer %#llx", (unsigned long long)f.inst_phys);
		CHECK(f.fault_address == 0x7FFF7F000000ULL, "fault address %#llx",
		    (unsigned long long)f.fault_address);
		CHECK(f.timestamp == 0x1deadbeefULL, "timestamp");
		CHECK(f.mmu_engine_id == 0x45 && f.fault_type == 2 && f.client_id == 0x18 &&
		    f.access_type == 2 && f.client_type == UVM_FAULT_CLIENT_HUB && f.gpc_id == 3,
		    "fields");
		CHECK(uvm_hal_blackwell_engine_type(0x45) == UVM_MMU_ENGINE_CE, "Blackwell 65..84 is CE");
		CHECK(uvm_hal_blackwell_engine_type(85) == UVM_MMU_ENGINE_HOST &&
		    uvm_hal_blackwell_engine_type(129) == UVM_MMU_ENGINE_HOST, "Blackwell 85..129 is HOST");
		CHECK(uvm_hal_blackwell_engine_type(384) == UVM_MMU_ENGINE_GRAPHICS &&
		    uvm_hal_blackwell_engine_type(400) == UVM_MMU_ENGINE_GRAPHICS, "graphics base 384");
		CHECK(uvm_hal_blackwell_engine_type(256) == UVM_MMU_ENGINE_BAR1 &&
		    uvm_hal_blackwell_engine_type(320) == UVM_MMU_ENGINE_BAR2, "BAR1/BAR2");
		/* GET/PUT registers: PTR 19:0, bits 30/31 getptr-corrupted and overflow */
		CHECK(uvm_fault_buffer_get_ptr(0xC00FFFFFu) == 0xFFFFF && uvm_fault_buffer_get_overflow(0x80000000u) &&
		    uvm_fault_buffer_get_write(5) == ((1u << 30) | (1u << 31) | 5u), "GET register fields");
		/* the 48-bit canonical form of a fault address */
		CHECK(uvm_canonical_address(0x0000800000000000ULL, 48) == 0xFFFF800000000000ULL &&
		    uvm_canonical_address(0x00007FFFFFFFF000ULL, 48) == 0x00007FFFFFFFF000ULL,
		    "canonical address");
	}

	/* 12 section 1: generation selection */
	{
		const struct uvm_hal_generation *g;

		g = uvm_hal_generation_by_arch(0x1B0);
		CHECK(g != NULL && g->mmu_version == 3 && g->max_channel_va == (1ULL << 57) &&
		    g->supported && g->needs_pb_segment_base && g->host_class == 0xCA6F && g->ce_class == 0xCAB5,
		    "Blackwell GB20x generation");
		g = uvm_hal_generation_by_arch(0x1A0);
		CHECK(g != NULL && !g->supported && g->mmu_version == 3, "Blackwell GB100 is a stub");
		g = uvm_hal_generation_by_arch(0x180);
		CHECK(g != NULL && !g->supported && g->max_channel_va == (1ULL << 57), "Hopper is a stub");
		g = uvm_hal_generation_by_arch(0x170);
		CHECK(g != NULL && !g->supported && g->mmu_version == 2 && g->max_channel_va == (1ULL << 40),
		    "Ampere is a stub");
		g = uvm_hal_generation_by_arch(0x190);
		CHECK(g != NULL && !g->supported && g->mmu_version == 2, "Ada is a stub");
		CHECK(uvm_hal_generation_by_arch(0x160) == NULL && uvm_hal_generation_by_arch(0) == NULL,
		    "unknown architectures");
		/* both class pairs RM may report for the GB206 are accepted */
		CHECK(uvm_hal_classes_supported(0x1B0, 0xCA6F, 0xCAB5) &&
		    uvm_hal_classes_supported(0x1B0, 0xC96F, 0xC9B5) &&
		    !uvm_hal_classes_supported(0x1B0, 0xC56F, 0xCAB5), "GB20x class pairs");
	}

	if (fails == 0)
		printf("hal_encode_test: PASS\n");
	return (fails != 0);
}
