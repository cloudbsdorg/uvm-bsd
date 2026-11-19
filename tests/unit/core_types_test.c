/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * core_types_test.c - the neutral vocabulary of sys/core/uvm_core_types.h
 * behaves as the core/backend split of docs/ARCHITECTURE.md needs it to:
 * the processor-id mask sets, clears and tests the boundary ids 0 and 256
 * as well as the ones between, the page sizes and the protections carry
 * the values the module already uses, the layout of a fault event is fixed
 * on LP64 so that a backend and the core agree on it, and the tracker
 * value 0 means no tracker.
 *
 * Red first: compiled before the header existed.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "core/uvm_core_types.h"

static int fails;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

/*
 * The layout of a fault event is part of the contract between the core
 * and a backend: the event is filled in by one and read by the other.
 * These are compile-time assertions, so a wrong layout fails the build
 * of this test rather than a run.
 */
_Static_assert(sizeof(struct uvm_core_fault_event) == 40, "fault event size");
_Static_assert(offsetof(struct uvm_core_fault_event, device_id) == 0, "fault event device_id");
_Static_assert(offsetof(struct uvm_core_fault_event, as) == 8, "fault event as");
_Static_assert(offsetof(struct uvm_core_fault_event, vaddr) == 16, "fault event vaddr");

/* #lizard forgives -- a list of independent checks, one branch each */
int
main(void)
{
	struct uvm_core_id_mask mask;
	struct uvm_core_phys_run run;
	struct uvm_core_fault_event event;
	uvm_core_tracker_t tracker;
	int id;

	/* ---- the ids: 0 is the CPU, 1..256 are the devices ---- */
	CHECK(UVM_CORE_ID_CPU == 0, "the CPU is id 0");
	CHECK(UVM_CORE_ID_FIRST == 1, "the first device is id 1");
	CHECK(UVM_CORE_ID_LAST == 256, "the last device is id 256");
	CHECK(UVM_CORE_ID_COUNT == 257, "there are 257 ids");

	/* ---- the mask ---- */
	memset(&mask, 0, sizeof(mask));
	CHECK(sizeof(mask.bits) == 5 * sizeof(uint64_t), "the mask holds 257 ids in 5 words");

	/* the boundary ids, the CPU's and the last device's */
	uvm_core_id_mask_set(&mask, UVM_CORE_ID_CPU);
	CHECK(uvm_core_id_mask_test(&mask, UVM_CORE_ID_CPU), "id 0 is set");
	CHECK(!uvm_core_id_mask_test(&mask, 1), "id 1 is not set yet");
	uvm_core_id_mask_clear(&mask, UVM_CORE_ID_CPU);
	CHECK(!uvm_core_id_mask_test(&mask, UVM_CORE_ID_CPU), "id 0 is cleared");

	uvm_core_id_mask_set(&mask, UVM_CORE_ID_LAST);
	CHECK(uvm_core_id_mask_test(&mask, UVM_CORE_ID_LAST), "id 256 is set");
	uvm_core_id_mask_clear(&mask, UVM_CORE_ID_LAST);
	CHECK(!uvm_core_id_mask_test(&mask, UVM_CORE_ID_LAST), "id 256 is cleared");

	/* the words that hold the boundary between the two */
	uvm_core_id_mask_set(&mask, 63);
	uvm_core_id_mask_set(&mask, 64);
	uvm_core_id_mask_set(&mask, 65);
	CHECK(uvm_core_id_mask_test(&mask, 63), "id 63 is set");
	CHECK(uvm_core_id_mask_test(&mask, 64), "id 64 is set");
	CHECK(uvm_core_id_mask_test(&mask, 65), "id 65 is set");
	uvm_core_id_mask_clear(&mask, 64);
	CHECK(!uvm_core_id_mask_test(&mask, 64), "id 64 is cleared");
	CHECK(uvm_core_id_mask_test(&mask, 63), "id 63 is still set");
	CHECK(uvm_core_id_mask_test(&mask, 65), "id 65 is still set");

	/* every id of the mask, set and cleared in turn */
	/* Time: O(257) sets and tests.  Space: O(1) */
	for (id = 0; id <= UVM_CORE_ID_LAST; id++) {
		uvm_core_id_mask_set(&mask, (uvm_core_id_t)id);
	}
	for (id = 0; id <= UVM_CORE_ID_LAST; id++) {
		CHECK(uvm_core_id_mask_test(&mask, (uvm_core_id_t)id), "id %d is set", id);
	}
	for (id = 0; id <= UVM_CORE_ID_LAST; id++) {
		uvm_core_id_mask_clear(&mask, (uvm_core_id_t)id);
	}
	for (id = 0; id <= UVM_CORE_ID_LAST; id++) {
		CHECK(!uvm_core_id_mask_test(&mask, (uvm_core_id_t)id), "id %d is cleared", id);
	}

	/* ---- the protections ---- */
	CHECK(UVM_CORE_PROT_NONE == 0, "no protection");
	CHECK(UVM_CORE_PROT_READ_ONLY == 1, "read only");
	CHECK(UVM_CORE_PROT_READ_WRITE == 2, "read write");
	CHECK(UVM_CORE_PROT_READ_WRITE_ATOMIC == 3, "read write atomic");

	/* ---- the apertures ---- */
	CHECK(UVM_CORE_APERTURE_LOCAL == 0, "a device-local aperture");
	CHECK(UVM_CORE_APERTURE_SYSTEM == 1, "a system aperture");
	CHECK(UVM_CORE_APERTURE_PEER0 == 2, "the first peer aperture");
	CHECK(UVM_CORE_APERTURE_PEER7 == 9, "the last peer aperture");

	/* ---- the page sizes ---- */
	CHECK(UVM_CORE_PAGE_SIZE_4K == 0x1000, "the 4K page size");
	CHECK(UVM_CORE_PAGE_SIZE_64K == 0x10000, "the 64K page size");
	CHECK(UVM_CORE_PAGE_SIZE_2M == 0x200000, "the 2M page size");
	CHECK(UVM_CORE_PAGE_SIZE_512M == 0x20000000, "the 512M page size");
	CHECK(uvm_core_page_size_bit(UVM_CORE_PAGE_SIZE_4K) == UVM_CORE_PAGE_SIZE_BIT_4K,
	    "the 4K page size bit");
	CHECK(uvm_core_page_size_bit(UVM_CORE_PAGE_SIZE_512M) == UVM_CORE_PAGE_SIZE_BIT_512M,
	    "the 512M page size bit");
	CHECK((UVM_CORE_PAGE_SIZE_BIT_4K | UVM_CORE_PAGE_SIZE_BIT_2M) ==
	    (1u << 0 | 1u << 2), "the page size bits");
	CHECK(uvm_core_page_size_supported(UVM_CORE_PAGE_SIZE_BIT_4K | UVM_CORE_PAGE_SIZE_BIT_64K,
	    UVM_CORE_PAGE_SIZE_64K), "the 64K page size is supported");
	CHECK(!uvm_core_page_size_supported(UVM_CORE_PAGE_SIZE_BIT_4K, UVM_CORE_PAGE_SIZE_64K),
	    "the 64K page size is not supported");

	/* ---- the trackers ---- */
	tracker = UVM_CORE_TRACKER_NONE;
	CHECK(tracker == 0, "no tracker is 0");
	CHECK(sizeof(tracker) == sizeof(uint64_t), "a tracker is a 64-bit handle");
	tracker = 0x1234567890abcdefULL;
	CHECK(tracker != UVM_CORE_TRACKER_NONE, "a tracker is opaque and not 0");

	/* ---- the physical runs ---- */
	run.address = 0x0000123456789000ULL;
	run.page_size = UVM_CORE_PAGE_SIZE_2M;
	run.aperture = UVM_CORE_APERTURE_SYSTEM;
	run.page_count = 4;
	CHECK(run.address == 0x0000123456789000ULL, "the address of a run");
	CHECK(run.page_size == UVM_CORE_PAGE_SIZE_2M, "the page size of a run");
	CHECK(run.aperture == UVM_CORE_APERTURE_SYSTEM, "the aperture of a run");
	CHECK(run.page_count == 4, "the page count of a run");

	/* ---- the fault events ---- */
	event.device_id = UVM_CORE_ID_FIRST;
	event.as = (void *)0x1000;
	event.vaddr = 0x0000000080000000ULL;
	event.access = UVM_CORE_FAULT_ACCESS_WRITE;
	event.fault_class = UVM_CORE_FAULT_CLASS_REPLAYABLE;
	event.token = 0xaabbccddULL;
	CHECK(event.device_id == UVM_CORE_ID_FIRST, "the device of a fault event");
	CHECK(event.as == (void *)0x1000, "the address space of a fault event");
	CHECK(event.vaddr == 0x0000000080000000ULL, "the address of a fault event");
	CHECK(event.access == UVM_CORE_FAULT_ACCESS_WRITE, "the access of a fault event");
	CHECK(event.fault_class == UVM_CORE_FAULT_CLASS_REPLAYABLE, "the class of a fault event");
	CHECK(event.token == 0xaabbccddULL, "the token of a fault event");
	CHECK(UVM_CORE_FAULT_ACCESS_READ == 0, "a read fault");
	CHECK(UVM_CORE_FAULT_ACCESS_WRITE == 1, "a write fault");
	CHECK(UVM_CORE_FAULT_ACCESS_ATOMIC == 2, "an atomic fault");
	CHECK(UVM_CORE_FAULT_ACCESS_PREFETCH == 3, "a prefetch fault");
	CHECK(UVM_CORE_FAULT_CLASS_REPLAYABLE == 0, "a replayable fault");
	CHECK(UVM_CORE_FAULT_CLASS_NON_REPLAYABLE == 1, "a non-replayable fault");

	if (fails == 0) {
		printf("ok   core_types\n");
		return 0;
	}
	printf("FAIL core_types: %d checks failed\n", fails);
	return 1;
}
