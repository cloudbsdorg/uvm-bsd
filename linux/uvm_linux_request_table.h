/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_linux_request_table.h - the one table that maps the driver's request
 * numbers onto the Linux request numbers (Track #2444).
 *
 * GENERATED FILE: do not edit it.  tools/gen-uvm-request-table.sh writes
 * it: the list of requests comes from NVIDIA's MIT uvm_ioctl.h, read by
 * tools/gen-ioctl-table.sh (SkunkWerks, BSD-2-Clause), and every request
 * is resolved against this repository - the driver's own request id (the
 * UVM_REQ_* enumeration of sys/nvidia_uvm_ioctl.h), the Linux request
 * number (LINUX_UVM_*) and the parameter structure the driver copies for
 * the request.  Only the names of the requests leave NVIDIA's header.
 *
 * Regenerate it with:
 *     make regen-ioctl-table NVIDIA_UVM_IOCTL_H=/path/to/uvm_ioctl.h
 *
 * X(name) is a request that takes a parameter structure and X0(name) the
 * one that takes none: the deinitialisation, which the driver answers
 * before its own lookup.
 */
#ifndef UVM_LINUX_REQUEST_TABLE_H
#define UVM_LINUX_REQUEST_TABLE_H

#define	UVM_LINUX_REQUEST_TABLE(X, X0)					\
	X(INITIALIZE)                       \
	X0(DEINITIALIZE)                     \
	X(CREATE_RANGE_GROUP)               \
	X(DESTROY_RANGE_GROUP)              \
	X(REGISTER_GPU_VASPACE)             \
	X(UNREGISTER_GPU_VASPACE)           \
	X(REGISTER_CHANNEL)                 \
	X(UNREGISTER_CHANNEL)               \
	X(ENABLE_PEER_ACCESS)               \
	X(DISABLE_PEER_ACCESS)              \
	X(SET_RANGE_GROUP)                  \
	X(MAP_EXTERNAL_ALLOCATION)          \
	X(FREE)                             \
	X(REGISTER_GPU)                     \
	X(UNREGISTER_GPU)                   \
	X(PAGEABLE_MEM_ACCESS)              \
	X(PREVENT_MIGRATION_RANGE_GROUPS)   \
	X(ALLOW_MIGRATION_RANGE_GROUPS)     \
	X(SET_PREFERRED_LOCATION)           \
	X(UNSET_PREFERRED_LOCATION)         \
	X(ENABLE_READ_DUPLICATION)          \
	X(DISABLE_READ_DUPLICATION)         \
	X(SET_ACCESSED_BY)                  \
	X(UNSET_ACCESSED_BY)                \
	X(MIGRATE)                          \
	X(MIGRATE_RANGE_GROUP)              \
	X(ENABLE_SYSTEM_WIDE_ATOMICS)       \
	X(DISABLE_SYSTEM_WIDE_ATOMICS)      \
	X(TOOLS_INIT_EVENT_TRACKER)         \
	X(TOOLS_SET_NOTIFICATION_THRESHOLD) \
	X(TOOLS_EVENT_QUEUE_ENABLE_EVENTS)  \
	X(TOOLS_EVENT_QUEUE_DISABLE_EVENTS) \
	X(TOOLS_ENABLE_COUNTERS)            \
	X(TOOLS_DISABLE_COUNTERS)           \
	X(TOOLS_READ_PROCESS_MEMORY)        \
	X(TOOLS_WRITE_PROCESS_MEMORY)       \
	X(TOOLS_GET_PROCESSOR_UUID_TABLE)   \
	X(MAP_DYNAMIC_PARALLELISM_REGION)   \
	X(UNMAP_EXTERNAL)                   \
	X(TOOLS_FLUSH_EVENTS)               \
	X(ALLOC_SEMAPHORE_POOL)             \
	X(CLEAN_UP_ZOMBIE_RESOURCES)        \
	X(PAGEABLE_MEM_ACCESS_ON_GPU)       \
	X(POPULATE_PAGEABLE)                \
	X(VALIDATE_VA_RANGE)                \
	X(CREATE_EXTERNAL_RANGE)            \
	X(MAP_EXTERNAL_SPARSE)              \
	X(MM_INITIALIZE)                    \
	X(TOOLS_INIT_EVENT_TRACKER_V2)      \
	X(TOOLS_GET_PROCESSOR_UUID_TABLE_V2)\
	X(ALLOC_DEVICE_P2P)                 \
	X(CLEAR_ALL_ACCESS_COUNTERS)        \
	X(DISCARD)                          \

#endif /* UVM_LINUX_REQUEST_TABLE_H */
