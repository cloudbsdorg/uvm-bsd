/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * debug_fmt_test.c - the hw.nvidia_uvm.debug_push formatters decode the
 * method stream and GPFIFO entries of uvm-spec r1 04 section 1.1 and 1.2,
 * and the encoders used by the Xid 71 probe produce the expected words.
 */

#include <stdio.h>
#include <string.h>

#include "uvm_hal_methods.h"
#include "uvm_debug.h"

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
	uint32_t w[32];
	char buf[160];
	size_t n, used;

	/* a CE semaphore release decodes as an incr group of 3 then a nincr launch */
	n = uvm_ce_semaphore_release(w, 0x0100000012345000ULL, 7, UVM_MEMBAR_SYS);
	used = uvm_debug_describe_method(buf, sizeof(buf), w, n);
	CHECK(used == 4, "CE semaphore group takes 4 words, got %zu", used);
	CHECK(strstr(buf, "subch 4") && strstr(buf, "method 0x240") && strstr(buf, "count 3"), "%s", buf);
	used = uvm_debug_describe_method(buf, sizeof(buf), w + 4, n - 4);
	CHECK(used == 2 && strstr(buf, "nincr") && strstr(buf, "method 0x300"), "%s", buf);

	/* host semaphore release */
	n = uvm_host_semaphore_release(w, 0x1000, 3);
	used = uvm_debug_describe_method(buf, sizeof(buf), w, n);
	CHECK(used == 6 && strstr(buf, "subch 0") && strstr(buf, "method 0x05c"), "%s", buf);

	/* a truncated group advances by one word and says so */
	used = uvm_debug_describe_method(buf, sizeof(buf), w, 3);
	CHECK(used == 1 && strstr(buf, "truncated"), "%s", buf);

	/* GPFIFO entries */
	uvm_debug_describe_gpfifo(buf, sizeof(buf), uvm_gpfifo_segment_base_entry(0x0100000000000000ULL));
	CHECK(strstr(buf, "control opcode 4") && strstr(buf, "0x10000"), "%s", buf);
	uvm_debug_describe_gpfifo(buf, sizeof(buf), uvm_gpfifo_entry(0x0000001234567800ULL, 12));
	CHECK(strstr(buf, "va 0x1234567800") && strstr(buf, "length 12 words") && strstr(buf, "sync 0"),
	    "%s", buf);

	if (fails == 0)
		printf("debug_fmt_test: all checks passed\n");
	return (fails ? 1 : 0);
}
