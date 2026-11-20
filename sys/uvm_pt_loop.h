/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_pt_loop.h - the pure parts of Track #2295, kept apart from the kernel
 * so that a userland test builds and runs the same code:
 *   - uvm_pt_vec_build: the map loop over the page tables of a range, with
 *     a yield (and, when interruptible, a pending-signal stop) between
 *     tables and a complete unwind of the references taken on a stop;
 *   - uvm_ratelimit_ok: the limiter behind uvm_log_slow;
 *   - uvm_sysmem_size_ok: the size rule of the GPU's one-page DMA tag.
 */

#ifndef UVM_PT_LOOP_H
#define UVM_PT_LOOP_H

#ifndef _KERNEL
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#endif
#include "nvidia_uvm_status.h"

/*
 * The GPU's DMA tag is as large as its largest allocation and no larger:
 * bus_dmamem_alloc allocates the tag's maxsize, so a bigger tag turns every
 * page table into a huge contiguous allocation (#2295).
 */
#define	UVM_GPU_DMA_MAXSIZE	PAGE_SIZE

static inline bool
uvm_sysmem_size_ok(size_t bytes)
{
	return (bytes != 0 && bytes <= UVM_GPU_DMA_MAXSIZE);
}

/* seconds between two lines of one uvm_log_slow call site */
#define	UVM_LOG_SLOW_SECS	10

/*
 * uvm_ratelimit_ok - true (and `last` updated) when `interval` has passed
 * since the last true answer, or on the first call (`last` zero).
 */
static inline bool
uvm_ratelimit_ok(struct timeval *last, const struct timeval *interval, const struct timeval *now)
{
	if (last->tv_sec != 0 || last->tv_usec != 0) {
		int64_t dsec = (int64_t)now->tv_sec - last->tv_sec;
		int64_t dusec = dsec * 1000000 + ((int64_t)now->tv_usec - last->tv_usec);

		if (dusec < (int64_t)interval->tv_sec * 1000000 + interval->tv_usec)
			return (false);
	}
	*last = *now;
	return (true);
}

struct uvm_pt_vec_ops {
	/* take the PTE range reference of table `n` for [va, va+piece) */
	NV_STATUS (*get)(void *ctx, unsigned n, uint64_t va, uint64_t piece);
	/* drop the reference of table `n` */
	void (*put)(void *ctx, unsigned n);
	/* yield; with `intr` also NV_ERR_SIGNAL_PENDING when a signal is pending */
	NV_STATUS (*yield)(void *ctx, bool intr);
};

/*
 * uvm_pt_vec_build - take the page tables of [va, va+size) one table span
 * at a time.  Before every table the loop yields; a signal is looked for
 * only when `intr` is set and at least one table is already held (so a map
 * of one table, and every non-interruptible build, always completes).  On
 * any failure the references taken so far are put back and *count is 0.
 *
 * Time: O(tables).  Space: O(1).
 */
static inline NV_STATUS
uvm_pt_vec_build(const struct uvm_pt_vec_ops *ops, void *ctx, uint64_t va, uint64_t size, uint64_t span,
    bool intr, unsigned *count)
{
	uint64_t cur = va, end = va + size;
	unsigned n = 0, i;
	NV_STATUS status;

	while (cur < end) {
		uint64_t table_end = (cur / span + 1) * span;
		uint64_t piece = (table_end < end ? table_end : end) - cur;

		status = ops->yield(ctx, intr && n > 0);
		if (status == NV_OK)
			status = ops->get(ctx, n, cur, piece);
		if (status != NV_OK) {
			for (i = 0; i < n; i++)
				ops->put(ctx, i);
			*count = 0;
			return (status);
		}
		n++;
		cur += piece;
	}
	*count = n;
	return (NV_OK);
}

#endif /* UVM_PT_LOOP_H */
