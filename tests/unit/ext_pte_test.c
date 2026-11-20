/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ext_pte_test.c - the PTE buffer RM fills for an external allocation
 * (sys/uvm_ext_pte.h).  RM reports the size of one entry it wrote
 * (UvmGpuExternalMappingInfo.pteSize, 02 section 3.8): 8 bytes for a PTE
 * at the PTE levels, 16 bytes for a 2M page, whose PTE lives in the dual
 * level of the page tree (05 section 1).  Live finding (CTX999-REPORT):
 * a buffer sized for 8 bytes per 2M page makes RM answer BUFFER_TOO_SMALL.
 *
 * Red first: compiled before the header existed.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "uvm_ext_pte.h"

static int fails;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

int
main(void)
{
	uint64_t buf[8];

	/* the entry size RM writes, by page size */
	CHECK(uvm_ext_pte_entry_size(4096) == 8, "4K entry size");
	CHECK(uvm_ext_pte_entry_size(65536) == 8, "64K entry size");
	CHECK(uvm_ext_pte_entry_size(2u << 20) == 16, "2M entry size");

	/* the size accepted from RM: the one the page size implies */
	CHECK(uvm_ext_pte_size_ok(4096, 8), "4K with 8");
	CHECK(!uvm_ext_pte_size_ok(4096, 16), "4K with 16");
	CHECK(uvm_ext_pte_size_ok(2u << 20, 16), "2M with 16");
	CHECK(!uvm_ext_pte_size_ok(2u << 20, 8), "2M with 8");
	CHECK(!uvm_ext_pte_size_ok(4096, 0), "size 0");

	/* 16-byte entries compact to their first word */
	buf[0] = 0xa1; buf[1] = 0; buf[2] = 0xa2; buf[3] = 0; buf[4] = 0xa3; buf[5] = 0;
	uvm_ext_pte_compact(buf, 16, 3);
	CHECK(buf[0] == 0xa1 && buf[1] == 0xa2 && buf[2] == 0xa3, "compact 16: %jx %jx %jx",
	    (uintmax_t)buf[0], (uintmax_t)buf[1], (uintmax_t)buf[2]);

	/* 8-byte entries are untouched */
	buf[0] = 1; buf[1] = 2; buf[2] = 3;
	uvm_ext_pte_compact(buf, 8, 3);
	CHECK(buf[0] == 1 && buf[1] == 2 && buf[2] == 3, "compact 8 changed the buffer");

	/* the dynamic-parallelism region: UMD passes a 4K length (live), 64K when it can */
	CHECK(uvm_sked_page_size(0x4f400000, 0x1000) == 4096, "sked 4K length");
	CHECK(uvm_sked_page_size(0x4f400000, 0x10000) == 65536, "sked 64K");
	CHECK(uvm_sked_page_size(0x4f401000, 0x10000) == 4096, "sked unaligned base");
	CHECK(uvm_sked_page_size(0x4f400000, 0x11000) == 4096, "sked odd length");
	CHECK(uvm_sked_page_size(0x4f400800, 0x1000) == 0, "sked base not 4K");
	CHECK(uvm_sked_page_size(0x4f400000, 0x800) == 0, "sked length not 4K");
	CHECK(uvm_sked_page_size(0x4f400000, 0) == 0, "sked zero length");

	if (fails == 0)
		printf("ext_pte_test: all passed\n");
	return (fails != 0);
}
