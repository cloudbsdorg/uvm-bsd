/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * rm_status_offset_test.c - the rmStatus field of every parameter structure
 * the module answers must be written where the ABI says it is, and read
 * from there by the user.
 *
 * The bug this test exists for: a driver that computes the status offset
 * as sizeof(struct) - sizeof(NV_STATUS) writes the status into the tail
 * padding of every structure whose alignment is 8 (uvm-spec r1,
 * 01-user-abi.md section 2.2 step 4 gives the offsets: UVM_INITIALIZE_PARAMS
 * has rmStatus at 8 in 16 bytes, UVM_CREATE_EXTERNAL_RANGE_PARAMS at 16 in
 * 24, UVM_MIGRATE_PARAMS at 72 in 80, UVM_PREVENT_MIGRATION_RANGE_GROUPS
 * at 264 in 272).  The user then reads the value the field held before the
 * call - zero, that is, NV_OK - and never sees the module's answer.
 *
 * The rule the module and this test share is UVM_RM_STATUS_OFFSET() in
 * sys/nvidia_uvm_ioctl.h.  This test checks three things per structure:
 * the ABI facts (the size and the rmStatus offset the specification
 * gives), that the shared rule agrees with the ABI, and that a status
 * written with the shared rule is the one a reader at the ABI's offset
 * sees.  The last check fails for any structure the shared rule gets
 * wrong, including a regression to the end-of-structure rule.
 *
 * Red first: before the fix the module had no shared rule at all (it
 * computed the offset inline), so this file did not compile.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "nvidia_uvm_ioctl.h"
#include "nvidia_uvm_status.h"

static int failures;

/*
 * Every parameter structure the module's request table answers, with the
 * size and the rmStatus offset uvm-spec r1 gives (01-user-abi.md section 5
 * and section 7.1).  A request added to the module's table needs a row
 * here: the table in sys/uvm_ioctl.c and this one must name the same
 * structures.
 */
#define	CASE(type, spec_size, spec_offset)					\
	{ #type, sizeof(type), (size_t)(spec_size), (size_t)(spec_offset),	\
	  offsetof(type, rmStatus), UVM_RM_STATUS_OFFSET(type) }

static const struct {
	const char	*name;
	size_t		size;
	size_t		spec_size;
	size_t		spec_offset;
	size_t		abi_offset;
	size_t		rule_offset;
} cases[] = {
	/* CPD-OFF -- one row per parameter structure: the repetition is the table */
	/* 5.1 initialisation family */
	CASE(UVM_INITIALIZE_PARAMS, 16, 8),
	CASE(UVM_MM_INITIALIZE_PARAMS, 8, 4),
	CASE(UVM_IS_8_SUPPORTED_PARAMS, 8, 4),
	CASE(UVM_PAGEABLE_MEM_ACCESS_PARAMS, 8, 4),
	CASE(UVM_PAGEABLE_MEM_ACCESS_ON_GPU_PARAMS, 24, 20),
	/* 5.2 GPU registration */
	CASE(UVM_REGISTER_GPU_PARAMS, 40, 36),
	CASE(UVM_UNREGISTER_GPU_PARAMS, 20, 16),
	/* 5.3 GPU VA space */
	CASE(UVM_REGISTER_GPU_VASPACE_PARAMS, 32, 28),
	CASE(UVM_UNREGISTER_GPU_VASPACE_PARAMS, 20, 16),
	/* 5.4 channels */
	CASE(UVM_REGISTER_CHANNEL_PARAMS, 56, 48),
	CASE(UVM_UNREGISTER_CHANNEL_PARAMS, 28, 24),
	/* 5.5 external ranges */
	CASE(UVM_CREATE_EXTERNAL_RANGE_PARAMS, 24, 16),
	CASE(UVM_MAP_EXTERNAL_ALLOCATION_PARAMS, 9264, 9260),
	CASE(UVM_MAP_EXTERNAL_SPARSE_PARAMS, 40, 32),
	CASE(UVM_UNMAP_EXTERNAL_PARAMS, 40, 32),
	CASE(UVM_FREE_PARAMS, 24, 16),
	CASE(UVM_VALIDATE_VA_RANGE_PARAMS, 24, 16),
	CASE(UVM_MAP_DYNAMIC_PARALLELISM_REGION_PARAMS, 40, 32),
	/* 5.6 semaphore pools */
	CASE(UVM_ALLOC_SEMAPHORE_POOL_PARAMS, 9248, 9240),
	/* 5.7 policy and migration */
	CASE(UVM_SET_PREFERRED_LOCATION_PARAMS, 40, 36),
	CASE(UVM_UNSET_PREFERRED_LOCATION_PARAMS, 24, 16),
	CASE(UVM_SET_ACCESSED_BY_PARAMS, 40, 32),
	CASE(UVM_UNSET_ACCESSED_BY_PARAMS, 40, 32),
	CASE(UVM_ENABLE_READ_DUPLICATION_PARAMS, 24, 16),
	CASE(UVM_DISABLE_READ_DUPLICATION_PARAMS, 24, 16),
	CASE(UVM_MIGRATE_PARAMS, 80, 72),
	CASE(UVM_MIGRATE_RANGE_GROUP_PARAMS, 32, 24),
	CASE(UVM_DISCARD_PARAMS, 32, 24),
	/* 5.8 range groups */
	CASE(UVM_CREATE_RANGE_GROUP_PARAMS, 16, 8),
	CASE(UVM_DESTROY_RANGE_GROUP_PARAMS, 16, 8),
	CASE(UVM_SET_RANGE_GROUP_PARAMS, 32, 24),
	CASE(UVM_PREVENT_MIGRATION_RANGE_GROUPS_PARAMS, 272, 264),
	CASE(UVM_ALLOW_MIGRATION_RANGE_GROUPS_PARAMS, 272, 264),
	/* 5.9 - 5.14 */
	CASE(UVM_ENABLE_SYSTEM_WIDE_ATOMICS_PARAMS, 20, 16),
	CASE(UVM_DISABLE_SYSTEM_WIDE_ATOMICS_PARAMS, 20, 16),
	CASE(UVM_CLEAN_UP_ZOMBIE_RESOURCES_PARAMS, 4, 0),
	CASE(UVM_POPULATE_PAGEABLE_PARAMS, 24, 20),
	CASE(UVM_TOOLS_READ_PROCESS_MEMORY_PARAMS, 40, 32),
	CASE(UVM_TOOLS_WRITE_PROCESS_MEMORY_PARAMS, 40, 32),
	CASE(UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_PARAMS, 16, 8),
	CASE(UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_V2_PARAMS, 16, 8),
	CASE(UVM_TOOLS_FLUSH_EVENTS_PARAMS, 4, 0),
	CASE(UVM_ENABLE_PEER_ACCESS_PARAMS, 36, 32),
	CASE(UVM_DISABLE_PEER_ACCESS_PARAMS, 36, 32),
	CASE(UVM_CLEAR_ALL_ACCESS_COUNTERS_PARAMS, 4, 0),
	CASE(UVM_ALLOC_DEVICE_P2P_PARAMS, 56, 52),
	/* 7.1 the tools node */
	CASE(UVM_TOOLS_INIT_EVENT_TRACKER_PARAMS, 56, 48),
	CASE(UVM_TOOLS_INIT_EVENT_TRACKER_V2_PARAMS, 56, 48),
	CASE(UVM_TOOLS_SET_NOTIFICATION_THRESHOLD_PARAMS, 8, 4),
	CASE(UVM_TOOLS_EVENT_QUEUE_ENABLE_EVENTS_PARAMS, 16, 8),
	CASE(UVM_TOOLS_EVENT_QUEUE_DISABLE_EVENTS_PARAMS, 16, 8),
	CASE(UVM_TOOLS_ENABLE_COUNTERS_PARAMS, 16, 8),
	CASE(UVM_TOOLS_DISABLE_COUNTERS_PARAMS, 16, 8),
	/* CPD-ON */
};

/*
 * dispatch_simulation - what the dispatcher does with a status, in userland:
 * the structure arrives zeroed, the module writes the status at the offset
 * its rule gives, and the user reads the field at the ABI's offset.  The
 * status the user sees must be the one the module wrote.
 */
static void
dispatch_simulation(const char *name, size_t abi_offset, size_t rule_offset)
{
	unsigned char buffer[sizeof(UVM_MAP_EXTERNAL_ALLOCATION_PARAMS)];
	NV_STATUS status = NV_ERR_INVALID_ARGUMENT;
	NV_STATUS seen;

	memset(buffer, 0, sizeof(buffer));
	memcpy(buffer + rule_offset, &status, sizeof(status));
	seen = *(NV_STATUS *)(void *)(buffer + abi_offset);
	if (seen != status) {
		printf("FAIL %s: the status written at %zu is read at %zu as %#x, not %#x\n",
		    name, rule_offset, abi_offset, seen, status);
		failures++;
	}
}

int
main(void)
{
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (cases[i].size != cases[i].spec_size) {
			printf("FAIL %s: size %zu, the specification says %zu\n",
			    cases[i].name, cases[i].size, cases[i].spec_size);
			failures++;
			continue;
		}
		if (cases[i].abi_offset != cases[i].spec_offset) {
			printf("FAIL %s: rmStatus at %zu, the specification says %zu\n",
			    cases[i].name, cases[i].abi_offset, cases[i].spec_offset);
			failures++;
			continue;
		}
		if (cases[i].rule_offset != cases[i].abi_offset) {
			printf("FAIL %s: the rule places the status at %zu, the ABI says %zu\n",
			    cases[i].name, cases[i].rule_offset, cases[i].abi_offset);
			failures++;
			continue;
		}
		if (cases[i].rule_offset + sizeof(NV_STATUS) > cases[i].size) {
			printf("FAIL %s: the status does not fit in the structure\n",
			    cases[i].name);
			failures++;
			continue;
		}
		dispatch_simulation(cases[i].name, cases[i].abi_offset, cases[i].rule_offset);
	}
	if (failures != 0) {
		printf("rm_status_offset_test: FAIL (%d failures)\n", failures);
		return (1);
	}
	printf("rm_status_offset_test: PASS (%zu structures)\n",
	    sizeof(cases) / sizeof(cases[0]));
	return (0);
}
