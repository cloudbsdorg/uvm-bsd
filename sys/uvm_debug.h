/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_debug.h - pure helpers for the hw.nvidia_uvm.debug_push diagnostics:
 * decode a method stream into one line per method group, and describe a
 * GPFIFO entry.  No kernel dependency, so tests/unit/debug_fmt_test.c
 * compiles it in userland.  Method header layout: uvm-spec r1, 04 section
 * 1.1; GPFIFO entry layout: 04 section 1.2.
 */

#ifndef _UVM_DEBUG_H_
#define	_UVM_DEBUG_H_

#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#endif

/* hw.nvidia_uvm.debug_push: how many pushes (per module life) are logged when non-zero */
#define	UVM_DEBUG_PUSH_MAX	64

/*
 * uvm_debug_describe_method - render the method group starting at w[0] into
 * buf and return how many words the group occupies (header plus data, or
 * 1 for a malformed or truncated group, so a caller always advances).
 * `avail` is the words left in the stream.
 */
static inline size_t
uvm_debug_describe_method(char *buf, size_t len, const uint32_t *w, size_t avail)
{
	uint32_t h = w[0];
	uint32_t op = h >> 29, count = (h >> 16) & 0x1FFFu, subch = (h >> 13) & 7u, addr = (h & 0xFFFu) << 2;
	size_t data = count, used, i, off;

	if (op == 4) {		/* immediate data: the count field holds the datum */
		snprintf(buf, len, "%08x imm   subch %u method %#05x data %#x", h, subch, addr, count);
		return (1);
	}
	if (op != 1 && op != 3 && op != 5) {
		snprintf(buf, len, "%08x opcode %u (not a data group)", h, op);
		return (1);
	}
	if (op == 5)
		data = count ? 1 : 0;
	used = 1 + data;
	if (used > avail) {
		snprintf(buf, len, "%08x truncated group: header says %zu data words, %zu left", h, data,
		    avail - 1);
		return (1);
	}
	off = (size_t)snprintf(buf, len, "%08x %-6s subch %u method %#05x count %zu:", h,
	    op == 1 ? "incr" : op == 3 ? "nincr" : "once", subch, addr, data);
	for (i = 0; i < data && off < len; i++)
		off += (size_t)snprintf(buf + off, len - off, " %08x", w[1 + i]);
	return (used);
}

/*
 * uvm_debug_describe_gpfifo - one GPFIFO entry (low word, high word): a
 * control entry (length 0 with a non-zero opcode) or a push reference.
 */
static inline void
uvm_debug_describe_gpfifo(char *buf, size_t len, uint64_t entry)
{
	uint32_t lo = (uint32_t)entry, hi = (uint32_t)(entry >> 32);
	uint32_t length = (hi >> 10) & 0x1FFFFFu;

	if (length == 0 && (hi & 0xFFu) != 0) {
		snprintf(buf, len, "%016jx control opcode %u operand %#x (segment base VA bits 56:40 = %#x)",
		    (uintmax_t)entry, hi & 0xFFu, lo, (lo >> 8) & 0x1FFFFu);
		return;
	}
	snprintf(buf, len, "%016jx push va %#jx length %u words level %u sync %u priv %u fetch %u",
	    (uintmax_t)entry, (uintmax_t)(((uint64_t)(hi & 0xFFu) << 32) | (lo & ~3u)), length,
	    (hi >> 9) & 1u, hi >> 31, (hi >> 8) & 1u, lo & 1u);
}

#endif /* _UVM_DEBUG_H_ */
