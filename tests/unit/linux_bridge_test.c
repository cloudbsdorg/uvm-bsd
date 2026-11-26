/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * linux_bridge_test.c - the request numbers of the driver and of the
 * optional Linux ioctl bridge (Track #2444), and the shape of the
 * driver's request table.
 *
 * The mapping between the driver's request numbers and the Linux ones is
 * ONE table: linux/uvm_linux_request_table.h, which
 * tools/gen-uvm-request-table.sh writes from NVIDIA's MIT uvm_ioctl.h.
 * This test walks that table instead of holding a second copy of it:
 *
 *   - every row must round-trip: the Linux number of a request translates
 *     to the driver's request and back;
 *   - the driver's request ids must be dense: every id but the
 *     deinitialisation is a request, and the request table covers each of
 *     them exactly once;
 *   - the native encoding must carry the id unchanged;
 *   - a number the driver has no request for must be refused.
 *
 * Every check is falsifiable: the last section feeds the rules the lists
 * they must refuse.
 */

#include <stdio.h>
#include <string.h>

#include "uvm_requests.h"
#include "../../linux/nvidia_uvm_linux_ioctl.h"

static int fails;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

/* the rows of the one table */
#define	NROWS	UVM_LINUX_REQUEST_COUNT

/*
 * check_ids - the driver's request ids are dense and every one of them is
 * a request.
 *
 * The deinitialisation is the one exception: it is answered before the
 * lookup, so it has no entry in the driver's own request table.
 */
static void
check_ids(void)
{
	uint32_t numbers[NROWS];
	size_t n = 0;
	size_t i;

	CHECK(NROWS + 1 == UVM_REQ_COUNT,
	    "every id but the deinitialisation is a request (%zu of %u)",
	    NROWS + 1, (unsigned)UVM_REQ_COUNT);
	CHECK(UVM_REQ_DEINITIALIZE == 2, "the deinitialisation is the second id");

	for (i = 0; i < NROWS; i++) {
		if (uvm_linux_requests[i].native == UVM_REQ_DEINITIALIZE)
			continue;
		numbers[n++] = uvm_linux_requests[i].native;
	}
	CHECK(uvm_request_numbers_dense(numbers, n, UVM_REQ_COUNT),
	    "the requests of the driver cover every id exactly once");
}

/*
 * check_native_encoding - the native encoding carries the id unchanged.
 */
static void
check_native_encoding(void)
{
	size_t i;

	for (i = 0; i < NROWS; i++)
		CHECK(uvm_ioctl_native_decode(UVM_IOCTL_NATIVE(uvm_linux_requests[i].native)) ==
		    uvm_linux_requests[i].native,
		    "the native encoding of the id %u decodes back to it",
		    (unsigned)uvm_linux_requests[i].native);
	/* a word that is not ours decodes to nothing */
	CHECK(uvm_ioctl_native_decode(0) == 0, "a zero word is not a request");
	CHECK(uvm_ioctl_native_decode(IOC_VOID | 0x5600 | 1) == 0, "another group is not a request");
	CHECK(uvm_ioctl_native_decode(UVM_IOCTL_NATIVE(UVM_REQ_NONE)) == UVM_REQ_NONE,
	    "the reserved number survives the encoding");
}

/*
 * check_round_trip - the two translations are inverses.
 */
static void
check_round_trip(void)
{
	size_t i;
	size_t j;

	for (i = 0; i < NROWS; i++) {
		CHECK(uvm_linux_request_id(uvm_linux_requests[i].linux) == uvm_linux_requests[i].native,
		    "the Linux number of %s translates to the driver's request",
		    uvm_linux_requests[i].name);
		CHECK(uvm_linux_request_number(uvm_linux_requests[i].native) == uvm_linux_requests[i].linux,
		    "the driver's request of %s translates back to the Linux number",
		    uvm_linux_requests[i].name);
	}
	/* every Linux number is a different one */
	for (i = 0; i < NROWS; i++)
		for (j = i + 1; j < NROWS; j++)
			CHECK(uvm_linux_requests[i].linux != uvm_linux_requests[j].linux,
			    "the Linux numbers of %s and %s are the same",
			    uvm_linux_requests[i].name, uvm_linux_requests[j].name);
}

/*
 * check_unknown_refused - a number the driver has no request for is
 * refused.
 *
 * The two initialisation requests carry a group byte of their own, so they
 * are the only Linux numbers no other request uses; they are pinned here
 * because NVIDIA's header does not list them (the build checks the rest
 * against it).
 */
static void
check_unknown_refused(void)
{
	CHECK(LINUX_UVM_INITIALIZE == 0x30000001, "the Linux initialise number");
	CHECK(LINUX_UVM_DEINITIALIZE == 0x30000002, "the Linux deinitialise number");
	CHECK(uvm_linux_request_id(LINUX_UVM_IS_8_SUPPORTED) == 0,
	    "the Linux number %u has no request of the driver",
	    (unsigned)LINUX_UVM_IS_8_SUPPORTED);
	CHECK(uvm_linux_request_id(0) == 0, "the number 0 has no request of the driver");
	CHECK(uvm_linux_request_number(UVM_REQ_COUNT) == 0,
	    "the id past the last one has no Linux number");
	CHECK(uvm_linux_request_number(UVM_REQ_NONE) == 0,
	    "the reserved id has no Linux number");
}

/*
 * check_refusal_cases - the rule of the request table must refuse the
 * lists it must refuse.
 */
static void
check_refusal_cases(void)
{
	uint32_t hole[NROWS];
	uint32_t twice[NROWS];
	uint32_t outside[NROWS];
	uint32_t deinit[NROWS];
	size_t n = 0;
	size_t i;

	for (i = 0; i < NROWS; i++) {
		if (uvm_linux_requests[i].native == UVM_REQ_DEINITIALIZE)
			continue;
		hole[n++] = uvm_linux_requests[i].native;
	}
	/* a hole: the last id is missing */
	CHECK(!uvm_request_numbers_dense(hole, n - 1, UVM_REQ_COUNT),
	    "a list with a hole is refused");
	/* a repeated id */
	for (i = 0; i < n; i++)
		twice[i] = hole[i];
	twice[0] = twice[1];
	CHECK(!uvm_request_numbers_dense(twice, n, UVM_REQ_COUNT),
	    "a list with a repeated id is refused");
	/* an id outside the range */
	for (i = 0; i < n; i++)
		outside[i] = hole[i];
	outside[0] = UVM_REQ_COUNT;
	CHECK(!uvm_request_numbers_dense(outside, n, UVM_REQ_COUNT),
	    "a list with an id past the last one is refused");
	outside[0] = 0;
	CHECK(!uvm_request_numbers_dense(outside, n, UVM_REQ_COUNT),
	    "a list with the number zero is refused");
	/* the deinitialisation has no entry */
	for (i = 0; i < n; i++)
		deinit[i] = hole[i];
	deinit[n - 1] = UVM_REQ_DEINITIALIZE;
	CHECK(!uvm_request_numbers_dense(deinit, n, UVM_REQ_COUNT),
	    "a list with the deinitialisation is refused");
}

int
main(void)
{
	check_ids();
	check_native_encoding();
	check_round_trip();
	check_unknown_refused();
	check_refusal_cases();

	if (fails == 0)
		printf("linux_bridge_test: all passed\n");
	return (fails != 0);
}
