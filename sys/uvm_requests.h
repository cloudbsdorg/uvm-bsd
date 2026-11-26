/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_requests.h - the shape of the request table of the driver, as pure
 * functions so that userland tests check every row.
 *
 * The request ids of the driver are dense: UVM_REQ_INITIALIZE is 1 and
 * every id after it is the next free number (nvidia_uvm_ioctl.h).  The
 * table in uvm_ioctl.c has one entry per request, and it is written by
 * hand, so a mistake in it is invisible: a missing id answers ENOSYS for
 * a request that exists, and a repeated id answers one request twice and
 * leaves another without an answer.
 *
 * The rule here is therefore that the table covers every id exactly once.
 * The deinitialisation is the one exception: it is answered before the
 * lookup (it takes no parameter and does nothing), so it has no entry.
 *
 * Time:  O(n) in the number of entries.  Space: O(1).
 */

#ifndef UVM_REQUESTS_H
#define UVM_REQUESTS_H

#ifndef _KERNEL
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#include "nvidia_uvm_ioctl.h"

/*
 * uvm_request_numbers_dense - does the list of request numbers cover every
 * request id exactly once?
 *
 * numbers holds the number of each entry of the table; count is the number
 * of request ids (the value of UVM_REQ_COUNT), so the ids run from 1 to
 * count - 1.  The answer is false when:
 *
 *   - a number is 0 or not below count (0 is not a request, and a number
 *     outside the range is a typo);
 *   - a number is the deinitialisation, which is answered before the
 *     lookup and must not have an entry;
 *   - a number appears twice;
 *   - an id has no number at all (a hole).
 */
static inline bool
uvm_request_numbers_dense(const uint32_t *numbers, size_t n, uint32_t count)
{
	uint64_t seen = 0;
	size_t i;
	uint32_t id;

	/* the ids fit in one word of the seen map */
	if (count > 64)
		return (false);
	for (i = 0; i < n; i++) {
		id = numbers[i];
		if (id == 0 || id >= count)
			return (false);
		if (id == UVM_REQ_DEINITIALIZE)
			return (false);
		if (seen & (1ULL << (id - 1)))
			return (false);
		seen |= 1ULL << (id - 1);
	}
	for (id = 1; id < count; id++) {
		if (id == UVM_REQ_DEINITIALIZE)
			continue;
		if (!(seen & (1ULL << (id - 1))))
			return (false);
	}
	return (true);
}

#endif /* UVM_REQUESTS_H */
