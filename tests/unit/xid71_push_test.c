/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * xid71_push_test.c - the page-table zeroing push that raised Xid 71 in test
 * boot 3 (physical system-memory memset, WFI, MEMBAR) encodes, word for word
 * and field by field, what the MIT class header clc8b5.h (inherited by
 * clcab5.h) and the host class header clc96f.h/clca6f.h define, and what
 * uvm-spec r1 04 section 1.3/1.4 describe.  The field positions below are
 * written out here from those headers, independently of uvm_hal_methods.h.
 */

#include <stdio.h>

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

#define	FIELD(v, hi, lo)	(((v) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))

int
main(void)
{
	uint32_t w[64];
	size_t n = 0;
	struct uvm_gpu_address dst;
	/* the observed stream of test boot 3 (push #10), first 20 words */
	static const uint32_t golden[20] = {
		0x20018099, 0x00000001,
		0x200381c0, 0x00000000, 0x00000000, 0x01030054,
		0x20028102, 0x00000809, 0xf2000000,
		0x20018106, 0x00000200,
		0x600180c0, 0x04002506,
		0x2001001e, 0x00000000,
		0x2004000a, 0x00000000, 0x00000000, 0x00000000, 0x28000000,
	};
	unsigned i;
	uint32_t launch, remap;

	dst.address = 0x809f2000000ULL;
	dst.is_virtual = 0;
	dst.aperture = UVM_APERTURE_SYS;
	n += uvm_ce_memset_8(w + n, &dst, 0, 4096, UVM_MEMBAR_SYS);
	n += uvm_host_wfi(w + n);
	n += uvm_host_membar(w + n, UVM_MEMBAR_SYS);
	CHECK(n == 20, "push is %zu words, expected 20 before the release", n);
	for (i = 0; i < 20 && i < n; i++)
		CHECK(w[i] == golden[i], "word %u: %08x, observed in boot 3 %08x", i, w[i], golden[i]);

	/* CPD-OFF -- one check per encoded field: the repetition is the decode table */
	/* SET_DST_PHYS_MODE 0x264: TARGET 1:0 = COHERENT_SYSMEM (1); PEER_ID 8:6 and FLA 9 clear */
	CHECK(FIELD(w[0], 11, 0) == (0x264u >> 2) && FIELD(w[0], 15, 13) == 4 && FIELD(w[0], 28, 16) == 1,
	    "SET_DST_PHYS_MODE header %08x", w[0]);
	CHECK(FIELD(w[1], 1, 0) == 1 && FIELD(w[1], 8, 6) == 0 && FIELD(w[1], 9, 9) == 0, "phys mode %08x", w[1]);
	/* SET_REMAP_CONST_A/B/COMPONENTS 0x700..0x708 */
	CHECK(FIELD(w[2], 11, 0) == (0x700u >> 2) && FIELD(w[2], 28, 16) == 3, "remap header %08x", w[2]);
	remap = w[5];
	CHECK(FIELD(remap, 2, 0) == 4, "DST_X CONST_A");
	CHECK(FIELD(remap, 6, 4) == 5, "DST_Y CONST_B");
	CHECK(FIELD(remap, 17, 16) == 3, "COMPONENT_SIZE FOUR");
	CHECK(FIELD(remap, 25, 24) == 1, "NUM_DST_COMPONENTS TWO");
	CHECK((remap & ~0x03030077u) == 0, "no stray remap bits: %08x", remap);
	/* OFFSET_OUT_UPPER 0x408 (UPPER 24:0), OFFSET_OUT_LOWER 0x40c */
	CHECK(FIELD(w[6], 11, 0) == (0x408u >> 2) && FIELD(w[6], 28, 16) == 2, "offset out header");
	CHECK(w[7] == (uint32_t)(0x809f2000000ULL >> 32) && FIELD(w[7], 31, 25) == 0, "OFFSET_OUT_UPPER %08x", w[7]);
	CHECK(w[8] == 0xf2000000u, "OFFSET_OUT_LOWER %08x", w[8]);
	/* LINE_LENGTH_IN 0x418: 512 elements of two 4-byte components = 4096 bytes */
	CHECK(FIELD(w[9], 11, 0) == (0x418u >> 2) && w[10] * 8 == 4096, "LINE_LENGTH_IN %u", w[10]);
	/* LAUNCH_DMA 0x300 (non-incrementing) */
	CHECK(FIELD(w[11], 11, 0) == (0x300u >> 2) && FIELD(w[11], 31, 29) == 3, "LAUNCH header %08x", w[11]);
	launch = w[12];
	CHECK(FIELD(launch, 1, 0) == 2, "DATA_TRANSFER_TYPE NON_PIPELINED");
	CHECK(FIELD(launch, 2, 2) == 1, "FLUSH_ENABLE");
	CHECK(FIELD(launch, 4, 3) == 0, "no semaphore");
	CHECK(FIELD(launch, 7, 7) == 0, "src layout (unused with remap, left block)");
	CHECK(FIELD(launch, 8, 8) == 1, "DST_MEMORY_LAYOUT PITCH");
	CHECK(FIELD(launch, 9, 9) == 0, "MULTI_LINE_ENABLE FALSE");
	CHECK(FIELD(launch, 10, 10) == 1, "REMAP_ENABLE");
	CHECK(FIELD(launch, 12, 12) == 0, "SRC_TYPE (unused)");
	CHECK(FIELD(launch, 13, 13) == 1, "DST_TYPE PHYSICAL");
	CHECK(FIELD(launch, 25, 25) == 0, "FLUSH_TYPE SYS");
	CHECK(FIELD(launch, 26, 26) == 1, "DISABLE_PLC");
	CHECK(FIELD(launch, 27, 27) == 0, "payload size ONE_WORD");
	CHECK(launch == 0x04002506u, "LAUNCH_DMA %08x", launch);
	/* host: WFI 0x78 on subchannel 0; MEM_OP_A..D 0x28..0x34, D bits 31:27 = 5 (MEMBAR), C bits 2:0 = 0 (system) */
	CHECK(FIELD(w[13], 11, 0) == (0x78u >> 2) && FIELD(w[13], 15, 13) == 0, "WFI");
	CHECK(FIELD(w[15], 11, 0) == (0x28u >> 2) && FIELD(w[15], 28, 16) == 4 && FIELD(w[15], 15, 13) == 0,
	    "MEM_OP header");
	CHECK(FIELD(w[19], 31, 27) == 5 && FIELD(w[18], 2, 0) == 0, "MEMBAR system");
	if (fails == 0)
		printf("xid71_push_test: ok\n");
	return (fails != 0);
	/* CPD-ON */
}
