/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pt_loop_test.c - behaviour of the long page-table loops of Track #2295,
 * built from the module's own uvm_pt_loop.h with fakes for the page tree,
 * maybe_yield() and the pending-signal check:
 *   - the map loop yields before every table and stops on a signal, but
 *     never before the first table;
 *   - on a stop every reference taken is put back (counts return to their
 *     starting values, no table leaks);
 *   - a non-interruptible build (the split of a partial unmap) completes
 *     whatever the signal state, so the surviving parts are never lost;
 *   - the failure of one table unwinds the earlier ones;
 *   - uvm_log_slow's limiter passes one message per interval;
 *   - the DMA size rule refuses 0 and anything above one page.
 */

#include <stdio.h>
#include <string.h>

#define	PAGE_SIZE	4096
#include "uvm_pt_loop.h"

static int fails;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

#define	MAXT	16
struct fake {
	int held[MAXT];		/* references held per table slot */
	unsigned gets, puts, yields;
	int signal_pending;	/* what sig_intr() would say */
	unsigned fail_at;	/* get() number that fails (0 = never) */
};

static NV_STATUS
f_get(void *ctx, unsigned n, uint64_t va, uint64_t piece)
{
	struct fake *f = ctx;

	(void)va; (void)piece;
	f->gets++;
	if (f->fail_at != 0 && f->gets == f->fail_at)
		return (NV_ERR_NO_MEMORY);
	f->held[n]++;
	return (NV_OK);
}

static void
f_put(void *ctx, unsigned n)
{
	struct fake *f = ctx;

	f->puts++;
	f->held[n]--;
}

/* the module's uvm_pt_yield: always yields, reports a signal only when asked to look */
static NV_STATUS
f_yield(void *ctx, bool intr)
{
	struct fake *f = ctx;

	f->yields++;
	return (intr && f->signal_pending ? NV_ERR_SIGNAL_PENDING : NV_OK);
}

static const struct uvm_pt_vec_ops ops = { f_get, f_put, f_yield };
#define	SPAN	(1ULL << 21)

static int
leaked(const struct fake *f)
{
	int i, n = 0;

	for (i = 0; i < MAXT; i++)
		n += f->held[i] != 0;
	return (n);
}

int
main(void)
{
	struct fake f;
	unsigned n;
	NV_STATUS st;
	struct timeval last, iv = { UVM_LOG_SLOW_SECS, 0 }, now;

	/* 5 tables, no signal: all built, one yield per table, no puts */
	memset(&f, 0, sizeof(f));
	st = uvm_pt_vec_build(&ops, &f, 0, 5 * SPAN, SPAN, true, &n);
	CHECK(st == NV_OK && n == 5 && f.gets == 5 && f.puts == 0, "plain build: %d n=%u", (int)st, n);
	CHECK(f.yields == 5, "one yield per table, got %u", f.yields);

	/* signal pending, interruptible: stops after the first table, unwinds it */
	memset(&f, 0, sizeof(f));
	f.signal_pending = 1;
	st = uvm_pt_vec_build(&ops, &f, 0, 5 * SPAN, SPAN, true, &n);
	CHECK(st == NV_ERR_SIGNAL_PENDING, "signal must stop the map: %d", (int)st);
	CHECK(n == 0, "nothing is kept on a stop: n=%u", n);
	CHECK(f.gets == 1 && f.puts == 1 && leaked(&f) == 0, "references back to start: gets %u puts %u leaked %d",
	    f.gets, f.puts, leaked(&f));

	/* the first table is never refused: a one-table map with a signal pending succeeds */
	memset(&f, 0, sizeof(f));
	f.signal_pending = 1;
	st = uvm_pt_vec_build(&ops, &f, 0, SPAN, SPAN, true, &n);
	CHECK(st == NV_OK && n == 1, "one-table map with a signal: %d n=%u", (int)st, n);

	/* non-interruptible (split of a partial unmap): completes with a signal pending */
	memset(&f, 0, sizeof(f));
	f.signal_pending = 1;
	st = uvm_pt_vec_build(&ops, &f, 0, 5 * SPAN, SPAN, false, &n);
	CHECK(st == NV_OK && n == 5 && f.puts == 0, "non-interruptible build was interrupted: %d n=%u", (int)st, n);

	/* a failing table unwinds the earlier ones */
	memset(&f, 0, sizeof(f));
	f.fail_at = 4;
	st = uvm_pt_vec_build(&ops, &f, 0, 5 * SPAN, SPAN, true, &n);
	CHECK(st == NV_ERR_NO_MEMORY && f.puts == 3 && leaked(&f) == 0, "get failure: %d puts %u leaked %d", (int)st,
	    f.puts, leaked(&f));

	/* an unaligned start: pieces end at table boundaries */
	memset(&f, 0, sizeof(f));
	st = uvm_pt_vec_build(&ops, &f, SPAN / 2, 2 * SPAN, SPAN, true, &n);
	CHECK(st == NV_OK && n == 3, "unaligned start covers 3 tables, got %u", n);

	/* the log limiter: first message passes, the rest of the interval is held back, then one more passes */
	memset(&last, 0, sizeof(last));
	now.tv_sec = 100; now.tv_usec = 0;
	CHECK(uvm_ratelimit_ok(&last, &iv, &now), "first message must pass");
	now.tv_sec = 101;
	CHECK(!uvm_ratelimit_ok(&last, &iv, &now), "1 s later must be held back");
	now.tv_sec = 109; now.tv_usec = 999999;
	CHECK(!uvm_ratelimit_ok(&last, &iv, &now), "just under the interval must be held back");
	now.tv_sec = 110; now.tv_usec = 0;
	CHECK(uvm_ratelimit_ok(&last, &iv, &now), "a full interval later must pass");
	CHECK(!uvm_ratelimit_ok(&last, &iv, &now), "and then be held back again");
	CHECK(UVM_LOG_SLOW_SECS >= 10, "the slow limit is at least 10 s");

	/* the DMA size rule */
	CHECK(!uvm_sysmem_size_ok(0), "0 bytes refused");
	CHECK(uvm_sysmem_size_ok(1), "1 byte ok");
	CHECK(uvm_sysmem_size_ok(PAGE_SIZE), "one page ok");
	CHECK(!uvm_sysmem_size_ok(PAGE_SIZE + 1), "more than a page refused");
	CHECK(!uvm_sysmem_size_ok(2 * PAGE_SIZE), "two pages refused");
	CHECK(!uvm_sysmem_size_ok(64u << 20), "64 MiB refused");

	if (fails == 0)
		printf("pt_loop_test: all passed\n");
	return (fails != 0);
}
