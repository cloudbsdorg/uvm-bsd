/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * nvidia_uvm_ioctl.h - the user ABI of /dev/nvidia-uvm and
 * /dev/nvidia-uvm-tools: request numbers, parameter structures (sizes,
 * alignments and offsets exactly as uvm-spec r1, 01-user-abi.md, gives
 * them), the primitive types and the tools queue records.  Field names
 * are spelled as the user-mode driver spells them: they are the ABI.
 *
 * Usable from the kernel and from userland.  Every 64-bit member carries
 * an explicit 8-byte alignment attribute (01 section 2.5): the layouts
 * below are identical on amd64 and arm64 and tests/unit/abi_layout_test.c
 * asserts each size and offset at compile time.
 */

#ifndef _NVIDIA_UVM_IOCTL_H_
#define	_NVIDIA_UVM_IOCTL_H_

#include <sys/types.h>
#ifndef _KERNEL
#include <stddef.h>
#include <stdint.h>
#endif
#ifdef __FreeBSD__
#include <sys/ioccom.h>
#endif

#include "nvidia_uvm_status.h"

/* 2.5 primitive types */
typedef uint8_t		NvU8;
typedef uint16_t	NvU16;
typedef uint32_t	NvU32;
typedef uint64_t	NvU64;
typedef int32_t		NvS32;
typedef int64_t		NvS64;
typedef uint8_t		NvBool;
typedef uint32_t	NvHandle;
typedef uint64_t	NvP64;
typedef uint64_t	NvLength;

#define	NV_TRUE		((NvBool)1)
#define	NV_FALSE	((NvBool)0)

#define	UVM_ALIGN8	__attribute__((aligned(8)))

/*
 * UVM_RM_STATUS_OFFSET - where the rmStatus field sits inside a parameter
 * structure.
 *
 * It is NOT sizeof(type) - sizeof(NV_STATUS).  A parameter structure whose
 * alignment is 8 and whose last field before rmStatus is 64-bit wide ends
 * with four bytes of tail padding, so the field the user reads is at
 * sizeof(type) - 8, not at sizeof(type) - 4 (uvm-spec r1, 01-user-abi.md
 * section 2.2 step 4 and the layouts of section 5: UVM_INITIALIZE_PARAMS
 * has rmStatus at 8 in 16 bytes, UVM_CREATE_EXTERNAL_RANGE_PARAMS at 16 in
 * 24, UVM_MIGRATE_PARAMS at 72 in 80).  A driver that writes the status at
 * the end of the structure writes it into that padding; the user then reads
 * the value the field had before the call and never sees the answer.
 *
 * The same rule is the only one both the module and the userland tests can
 * use, so it lives here.
 */
#ifdef _KERNEL
#define	UVM_RM_STATUS_OFFSET(type)	((size_t)__offsetof(type, rmStatus))
#else
#define	UVM_RM_STATUS_OFFSET(type)	((size_t)offsetof(type, rmStatus))
#endif

typedef struct NvProcessorUuid {
	NvU8	uuid[16];
} NvProcessorUuid;

/*
 * The CPU's well-known processor UUID (01 section 2.5): the module only
 * compares for equality with it and writes it to index 0 of the tools
 * UUID table.
 */
#define	NV_PROCESSOR_UUID_CPU_DEFAULT_INITIALIZER			\
	{ { 0xa6, 0x5e, 0x0f, 0x4e, 0xd7, 0xd4, 0x7b, 0xa2,		\
	    0x50, 0x47, 0x41, 0x2c, 0x14, 0x2a, 0x77, 0x73 } }
static const NvProcessorUuid NV_PROCESSOR_UUID_CPU_DEFAULT =
    NV_PROCESSOR_UUID_CPU_DEFAULT_INITIALIZER;

/* enumerations carried in NvU32 fields */
enum {
	UvmGpuMappingTypeDefault = 0,
	UvmGpuMappingTypeReadWriteAtomic = 1,
	UvmGpuMappingTypeReadWrite = 2,
	UvmGpuMappingTypeReadOnly = 3,
	UvmGpuMappingTypeCount = 4
};
enum {
	UvmGpuCachingTypeDefault = 0,
	UvmGpuCachingTypeForceUncached = 1,
	UvmGpuCachingTypeForceCached = 2,
	UvmGpuCachingTypeCount = 3
};
enum {
	UvmGpuFormatTypeDefault = 0,
	UvmGpuFormatTypeBlockLinear = 1,
	UvmGpuFormatTypeCount = 2
};
enum {
	UvmGpuFormatElementBitsDefault = 0,
	UvmGpuFormatElementBits8 = 1,
	UvmGpuFormatElementBits16 = 2,
	/* 3 is not valid */
	UvmGpuFormatElementBits32 = 4,
	UvmGpuFormatElementBits64 = 5,
	UvmGpuFormatElementBits128 = 6,
	UvmGpuFormatElementBitsCount = 7
};
enum {
	UvmGpuCompressionTypeDefault = 0,
	UvmGpuCompressionTypeEnabledNoPlc = 1,
	UvmGpuCompressionTypeCount = 2
};

typedef struct UvmGpuMappingAttributes {
	NvProcessorUuid	gpuUuid;
	NvU32		gpuMappingType;
	NvU32		gpuCachingType;
	NvU32		gpuFormatType;
	NvU32		gpuElementBits;
	NvU32		gpuCompressionType;
} UvmGpuMappingAttributes;

#define	UVM_MAX_GPUS		256
#define	UVM_MAX_GPUS_V1		32
#define	UVM_MAX_RANGE_GROUPS_REQUEST_SIZE	32

/* 3.3 UVM_INITIALIZE flags */
#define	UVM_INIT_FLAGS_DISABLE_HMM			((NvU64)1 << 0)
#define	UVM_INIT_FLAGS_MULTI_PROCESS_SHARING_MODE	((NvU64)1 << 1)
#define	UVM_INIT_FLAGS_MASK	(UVM_INIT_FLAGS_DISABLE_HMM |		\
	UVM_INIT_FLAGS_MULTI_PROCESS_SHARING_MODE)

/* 5.7 UVM_MIGRATE flags */
#define	UVM_MIGRATE_FLAG_ASYNC		((NvU32)1 << 0)
#define	UVM_MIGRATE_FLAGS_TEST_ALL	((NvU32)3 << 1)	/* test-only bits */
#define	UVM_DISCARD_FLAGS_UNMAP		((NvU64)1)

/*
 * The request numbers of the driver.
 *
 * These are the driver's own: they are not the numbers a Linux binary
 * issues.  A native FreeBSD caller (our libcuda, Track #2428) uses them
 * directly; a third-party Linux CUDA binary reaches them through the
 * optional Linux ioctl bridge (nvidia_uvm_linux.ko), which owns the Linux
 * numbers and translates them into these.
 *
 * The numbers are dense and stable: a request keeps its number for as long
 * as it exists, so that a caller built against one revision of this header
 * keeps working.  A new request takes the next free number; a removed one
 * leaves a hole rather than moving the ones after it.
 */
enum uvm_request_id {
	UVM_REQ_INITIALIZE = 1,
	UVM_REQ_DEINITIALIZE,
	UVM_REQ_MM_INITIALIZE,
	UVM_REQ_PAGEABLE_MEM_ACCESS,
	UVM_REQ_PAGEABLE_MEM_ACCESS_ON_GPU,
	UVM_REQ_REGISTER_GPU,
	UVM_REQ_UNREGISTER_GPU,
	UVM_REQ_REGISTER_GPU_VASPACE,
	UVM_REQ_UNREGISTER_GPU_VASPACE,
	UVM_REQ_REGISTER_CHANNEL,
	UVM_REQ_UNREGISTER_CHANNEL,
	UVM_REQ_ENABLE_PEER_ACCESS,
	UVM_REQ_DISABLE_PEER_ACCESS,
	UVM_REQ_CREATE_RANGE_GROUP,
	UVM_REQ_DESTROY_RANGE_GROUP,
	UVM_REQ_SET_RANGE_GROUP,
	UVM_REQ_PREVENT_MIGRATION_RANGE_GROUPS,
	UVM_REQ_ALLOW_MIGRATION_RANGE_GROUPS,
	UVM_REQ_CREATE_EXTERNAL_RANGE,
	UVM_REQ_MAP_EXTERNAL_ALLOCATION,
	UVM_REQ_MAP_EXTERNAL_SPARSE,
	UVM_REQ_UNMAP_EXTERNAL,
	UVM_REQ_FREE,
	UVM_REQ_VALIDATE_VA_RANGE,
	UVM_REQ_MAP_DYNAMIC_PARALLELISM_REGION,
	UVM_REQ_ALLOC_SEMAPHORE_POOL,
	UVM_REQ_SET_PREFERRED_LOCATION,
	UVM_REQ_UNSET_PREFERRED_LOCATION,
	UVM_REQ_SET_ACCESSED_BY,
	UVM_REQ_UNSET_ACCESSED_BY,
	UVM_REQ_ENABLE_READ_DUPLICATION,
	UVM_REQ_DISABLE_READ_DUPLICATION,
	UVM_REQ_MIGRATE,
	UVM_REQ_MIGRATE_RANGE_GROUP,
	UVM_REQ_DISCARD,
	UVM_REQ_ENABLE_SYSTEM_WIDE_ATOMICS,
	UVM_REQ_DISABLE_SYSTEM_WIDE_ATOMICS,
	UVM_REQ_CLEAN_UP_ZOMBIE_RESOURCES,
	UVM_REQ_POPULATE_PAGEABLE,
	UVM_REQ_TOOLS_READ_PROCESS_MEMORY,
	UVM_REQ_TOOLS_WRITE_PROCESS_MEMORY,
	UVM_REQ_TOOLS_GET_PROCESSOR_UUID_TABLE,
	UVM_REQ_TOOLS_GET_PROCESSOR_UUID_TABLE_V2,
	UVM_REQ_TOOLS_FLUSH_EVENTS,
	UVM_REQ_CLEAR_ALL_ACCESS_COUNTERS,
	UVM_REQ_ALLOC_DEVICE_P2P,
	UVM_REQ_TOOLS_INIT_EVENT_TRACKER,
	UVM_REQ_TOOLS_INIT_EVENT_TRACKER_V2,
	UVM_REQ_TOOLS_SET_NOTIFICATION_THRESHOLD,
	UVM_REQ_TOOLS_EVENT_QUEUE_ENABLE_EVENTS,
	UVM_REQ_TOOLS_EVENT_QUEUE_DISABLE_EVENTS,
	UVM_REQ_TOOLS_ENABLE_COUNTERS,
	UVM_REQ_TOOLS_DISABLE_COUNTERS,
	UVM_REQ_COUNT
};

/*
 * UVM_REQ_NONE - the reserved request number that means "no request".
 *
 * The driver answers it as it answers any other number it does not know:
 * ENOSYS on the main node, EINVAL on the tools node (01 section 2.2).  The
 * optional Linux ioctl bridge uses it for a Linux request number that has
 * no driver request behind it, so that the answer is the driver's own.
 */
#define	UVM_REQ_NONE	0xffu

/*
 * The native FreeBSD request encoding.
 *
 * A request word carries the direction and the size in the high bits, the
 * group in the next byte and the request number in the low eight bits
 * (sys/ioccom.h).  This driver uses the VOID direction with a zero size
 * and its own group byte ('U'): the shape of _IO('U', n).  With a zero
 * size the kernel hands the driver the user's argument word itself, so the
 * parameter pointer reaches the driver unchanged and the driver copies the
 * structure in and out itself (01 section 2.2).
 *
 * The layout of the word (sys/ioccom.h):
 *
 *   bits 31-29  the direction: IOC_VOID is 0x20000000, and the direction
 *               bits of anything else are not ours;
 *   bits 28-16  the size in bytes: it must be zero, because the driver
 *               copies the structures itself;
 *   bits 15-8   the group: ours is 'U';
 *   bits 7-0    the request number.
 *
 * The number is in the low eight bits, so the encoding carries at most
 * 256 requests; the driver has fewer than a hundred.
 */
#define	UVM_IOCTL_NATIVE_GROUP	0x55	/* 'U' */
#ifdef __FreeBSD__
#define	UVM_IOCTL_NATIVE(id)						\
	((unsigned long)(IOC_VOID | ((unsigned long)UVM_IOCTL_NATIVE_GROUP << 8) | \
	    ((unsigned long)(id) & 0xffu)))
#else
#define	UVM_IOCTL_NATIVE(id)	((unsigned long)(id))
#endif

/*
 * uvm_ioctl_native_decode - the request number behind a native FreeBSD
 * request word, or 0 (never a valid number) when the word is not one
 * UVM_IOCTL_NATIVE() produces.
 *
 * Every field of the word is checked: the direction must be the void
 * direction, the size must be zero, and the group must be ours.  A word
 * that passes the checks carries the request number in its low eight bits.
 *
 * Time:  O(1).  Space: O(1).
 */
static inline NvU32
uvm_ioctl_native_decode(unsigned long cmd)
{
	NvU32 c = (NvU32)cmd;

	/* the direction: the void direction and nothing else */
	if ((c & 0xe0000000u) != ((NvU32)IOC_VOID & 0xe0000000u))
		return (0);
	/* the size: none, the driver copies the structures itself */
	if ((c & 0x1fff0000u) != 0)
		return (0);
	/* the group: ours */
	if (((c >> 8) & 0xffu) != (NvU32)UVM_IOCTL_NATIVE_GROUP)
		return (0);
	return (c & 0xffu);
}

/* 5.1 initialisation family */
typedef struct {
	NvU64		flags UVM_ALIGN8;	/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_INITIALIZE_PARAMS;

typedef struct {
	NvS32		uvmFd;			/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_MM_INITIALIZE_PARAMS;

typedef struct {
	NvU32		is8Supported;		/* OUT */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_IS_8_SUPPORTED_PARAMS;

typedef struct {
	NvBool		pageableMemAccess;	/* OUT */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_PAGEABLE_MEM_ACCESS_PARAMS;

typedef struct {
	NvProcessorUuid	gpu_uuid;		/* IN */
	NvBool		pageableMemAccess;	/* OUT */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_PAGEABLE_MEM_ACCESS_ON_GPU_PARAMS;

/* 5.2 GPU registration */
typedef struct {
	NvProcessorUuid	gpu_uuid;		/* IN/OUT */
	NvBool		numaEnabled;		/* OUT */
	NvS32		numaNodeId;		/* OUT */
	NvS32		rmCtrlFd;		/* IN */
	NvHandle	hClient;		/* IN */
	NvHandle	hSmcPartRef;		/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_REGISTER_GPU_PARAMS;

typedef struct {
	NvProcessorUuid	gpu_uuid;		/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_UNREGISTER_GPU_PARAMS;

/* 5.3 GPU VA space registration */
typedef struct {
	NvProcessorUuid	gpuUuid;		/* IN */
	NvS32		rmCtrlFd;		/* IN */
	NvHandle	hClient;		/* IN */
	NvHandle	hVaSpace;		/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_REGISTER_GPU_VASPACE_PARAMS;

typedef struct {
	NvProcessorUuid	gpuUuid;		/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_UNREGISTER_GPU_VASPACE_PARAMS;

/* 5.4 channel registration */
typedef struct {
	NvProcessorUuid	gpuUuid;		/* IN */
	NvS32		rmCtrlFd;		/* IN */
	NvHandle	hClient;		/* IN */
	NvHandle	hChannel;		/* IN */
	NvU64		base UVM_ALIGN8;	/* IN */
	NvU64		length UVM_ALIGN8;	/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_REGISTER_CHANNEL_PARAMS;

typedef struct {
	NvProcessorUuid	gpuUuid;		/* IN (unused for lookup) */
	NvHandle	hClient;		/* IN */
	NvHandle	hChannel;		/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_UNREGISTER_CHANNEL_PARAMS;

/* 5.12 peer access */
typedef struct {
	NvProcessorUuid	gpuUuidA;		/* IN */
	NvProcessorUuid	gpuUuidB;		/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_ENABLE_PEER_ACCESS_PARAMS;

typedef UVM_ENABLE_PEER_ACCESS_PARAMS UVM_DISABLE_PEER_ACCESS_PARAMS;

/* 5.8 range groups */
typedef struct {
	NvU64		rangeGroupId UVM_ALIGN8;	/* OUT */
	NV_STATUS	rmStatus;			/* OUT */
} UVM_CREATE_RANGE_GROUP_PARAMS;

typedef struct {
	NvU64		rangeGroupId UVM_ALIGN8;	/* IN */
	NV_STATUS	rmStatus;			/* OUT */
} UVM_DESTROY_RANGE_GROUP_PARAMS;

typedef struct {
	NvU64		rangeGroupId UVM_ALIGN8;	/* IN */
	NvU64		requestedBase UVM_ALIGN8;	/* IN */
	NvU64		length UVM_ALIGN8;		/* IN */
	NV_STATUS	rmStatus;			/* OUT */
} UVM_SET_RANGE_GROUP_PARAMS;

typedef struct {
	NvU64		rangeGroupIds[UVM_MAX_RANGE_GROUPS_REQUEST_SIZE] UVM_ALIGN8; /* IN */
	NvU64		numGroupIds UVM_ALIGN8;		/* IN */
	NV_STATUS	rmStatus;			/* OUT */
} UVM_PREVENT_MIGRATION_RANGE_GROUPS_PARAMS;

typedef UVM_PREVENT_MIGRATION_RANGE_GROUPS_PARAMS UVM_ALLOW_MIGRATION_RANGE_GROUPS_PARAMS;

/* 5.5 external ranges and allocations */
typedef struct {
	NvU64		base UVM_ALIGN8;	/* IN */
	NvU64		length UVM_ALIGN8;	/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_CREATE_EXTERNAL_RANGE_PARAMS;

typedef struct {
	NvU64		base UVM_ALIGN8;	/* IN */
	NvU64		length UVM_ALIGN8;	/* IN */
	NvU64		offset UVM_ALIGN8;	/* IN */
	UvmGpuMappingAttributes perGpuAttributes[UVM_MAX_GPUS];	/* IN */
	NvU64		gpuAttributesCount UVM_ALIGN8;	/* IN */
	NvS32		rmCtrlFd;		/* IN */
	NvU32		hClient;		/* IN */
	NvU32		hMemory;		/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_MAP_EXTERNAL_ALLOCATION_PARAMS;

typedef struct {
	NvU64		base UVM_ALIGN8;	/* IN */
	NvU64		length UVM_ALIGN8;	/* IN */
	NvProcessorUuid	gpuUuid;		/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_MAP_EXTERNAL_SPARSE_PARAMS;

typedef UVM_MAP_EXTERNAL_SPARSE_PARAMS UVM_UNMAP_EXTERNAL_PARAMS;
typedef UVM_MAP_EXTERNAL_SPARSE_PARAMS UVM_MAP_DYNAMIC_PARALLELISM_REGION_PARAMS;

typedef struct {
	NvU64		base UVM_ALIGN8;	/* IN */
	NvU64		length UVM_ALIGN8;	/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_FREE_PARAMS;

typedef UVM_FREE_PARAMS UVM_VALIDATE_VA_RANGE_PARAMS;

/* 5.6 semaphore pools */
typedef struct {
	NvU64		base UVM_ALIGN8;	/* IN */
	NvU64		length UVM_ALIGN8;	/* IN */
	UvmGpuMappingAttributes perGpuAttributes[UVM_MAX_GPUS];	/* IN */
	NvU64		gpuAttributesCount UVM_ALIGN8;	/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_ALLOC_SEMAPHORE_POOL_PARAMS;

/* 5.7 managed memory policy and migration */
typedef struct {
	NvU64		requestedBase UVM_ALIGN8;	/* IN */
	NvU64		length UVM_ALIGN8;		/* IN */
	NvProcessorUuid	preferredLocation;		/* IN */
	NvS32		preferredCpuNumaNode;		/* IN */
	NV_STATUS	rmStatus;			/* OUT */
} UVM_SET_PREFERRED_LOCATION_PARAMS;

typedef struct {
	NvU64		requestedBase UVM_ALIGN8;	/* IN */
	NvU64		length UVM_ALIGN8;		/* IN */
	NV_STATUS	rmStatus;			/* OUT */
} UVM_UNSET_PREFERRED_LOCATION_PARAMS;

typedef struct {
	NvU64		requestedBase UVM_ALIGN8;	/* IN */
	NvU64		length UVM_ALIGN8;		/* IN */
	NvProcessorUuid	accessedByUuid;			/* IN */
	NV_STATUS	rmStatus;			/* OUT */
} UVM_SET_ACCESSED_BY_PARAMS;

typedef UVM_SET_ACCESSED_BY_PARAMS UVM_UNSET_ACCESSED_BY_PARAMS;
typedef UVM_UNSET_PREFERRED_LOCATION_PARAMS UVM_ENABLE_READ_DUPLICATION_PARAMS;
typedef UVM_UNSET_PREFERRED_LOCATION_PARAMS UVM_DISABLE_READ_DUPLICATION_PARAMS;

typedef struct {
	NvU64		base UVM_ALIGN8;		/* IN */
	NvU64		length UVM_ALIGN8;		/* IN */
	NvProcessorUuid	destinationUuid;		/* IN */
	NvU32		flags;				/* IN */
	NvU64		semaphoreAddress UVM_ALIGN8;	/* IN */
	NvU32		semaphorePayload;		/* IN */
	NvS32		cpuNumaNode;			/* IN */
	NvU64		userSpaceStart UVM_ALIGN8;	/* OUT */
	NvU64		userSpaceLength UVM_ALIGN8;	/* OUT */
	NV_STATUS	rmStatus;			/* OUT */
} UVM_MIGRATE_PARAMS;

typedef struct {
	NvU64		rangeGroupId UVM_ALIGN8;	/* IN */
	NvProcessorUuid	destinationUuid;		/* IN */
	NV_STATUS	rmStatus;			/* OUT */
} UVM_MIGRATE_RANGE_GROUP_PARAMS;

typedef struct {
	NvU64		base UVM_ALIGN8;	/* IN */
	NvU64		length;			/* IN (naturally aligned) */
	NvU64		flags UVM_ALIGN8;	/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_DISCARD_PARAMS;

/* 5.9 system-wide atomics */
typedef struct {
	NvProcessorUuid	gpu_uuid;		/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_ENABLE_SYSTEM_WIDE_ATOMICS_PARAMS;

typedef UVM_ENABLE_SYSTEM_WIDE_ATOMICS_PARAMS UVM_DISABLE_SYSTEM_WIDE_ATOMICS_PARAMS;

/* 5.10 teardown helpers */
typedef struct {
	NV_STATUS	rmStatus;		/* OUT */
} UVM_CLEAN_UP_ZOMBIE_RESOURCES_PARAMS;

typedef struct {
	NvU64		base UVM_ALIGN8;	/* IN */
	NvU64		length UVM_ALIGN8;	/* IN */
	NvU32		flags;			/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_POPULATE_PAGEABLE_PARAMS;

/* 5.11 tools ioctls on the main node */
typedef struct {
	NvU64		buffer UVM_ALIGN8;	/* IN: user pointer */
	NvU64		size UVM_ALIGN8;	/* IN */
	NvU64		targetVa UVM_ALIGN8;	/* IN */
	NvU64		bytesRead UVM_ALIGN8;	/* OUT */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_TOOLS_READ_PROCESS_MEMORY_PARAMS;

typedef struct {
	NvU64		buffer UVM_ALIGN8;	/* IN: user pointer */
	NvU64		size UVM_ALIGN8;	/* IN */
	NvU64		targetVa UVM_ALIGN8;	/* IN */
	NvU64		bytesWritten UVM_ALIGN8;/* OUT */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_TOOLS_WRITE_PROCESS_MEMORY_PARAMS;

typedef struct {
	NvU64		tablePtr UVM_ALIGN8;	/* IN: user pointer */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_PARAMS;

typedef UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_PARAMS UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_V2_PARAMS;

typedef struct {
	NV_STATUS	rmStatus;		/* OUT */
} UVM_TOOLS_FLUSH_EVENTS_PARAMS;

/* 5.13 access counters */
typedef struct {
	NV_STATUS	rmStatus;		/* OUT */
} UVM_CLEAR_ALL_ACCESS_COUNTERS_PARAMS;

/* 5.14 device P2P memory */
typedef struct {
	NvU64		base UVM_ALIGN8;	/* IN */
	NvU64		length UVM_ALIGN8;	/* IN */
	NvU64		offset UVM_ALIGN8;	/* IN */
	NvProcessorUuid	gpuUuid;		/* IN */
	NvS32		rmCtrlFd;		/* IN */
	NvHandle	hClient;		/* IN */
	NvHandle	hMemory;		/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_ALLOC_DEVICE_P2P_PARAMS;

/* 7.1 the tools node */
typedef struct {
	NvU64		queueBuffer UVM_ALIGN8;		/* IN */
	NvU64		queueBufferSize UVM_ALIGN8;	/* IN: entries */
	NvU64		controlBuffer UVM_ALIGN8;	/* IN */
	NvProcessorUuid	processor;			/* IN */
	NvU32		allProcessors;			/* IN */
	NvU32		uvmFd;				/* IN */
	NV_STATUS	rmStatus;			/* OUT */
} UVM_TOOLS_INIT_EVENT_TRACKER_PARAMS;

typedef UVM_TOOLS_INIT_EVENT_TRACKER_PARAMS UVM_TOOLS_INIT_EVENT_TRACKER_V2_PARAMS;

typedef struct {
	NvU32		notificationThreshold;	/* IN */
	NV_STATUS	rmStatus;		/* OUT */
} UVM_TOOLS_SET_NOTIFICATION_THRESHOLD_PARAMS;

typedef struct {
	NvU64		eventTypeFlags UVM_ALIGN8;	/* IN */
	NV_STATUS	rmStatus;			/* OUT */
} UVM_TOOLS_EVENT_QUEUE_ENABLE_EVENTS_PARAMS;

typedef UVM_TOOLS_EVENT_QUEUE_ENABLE_EVENTS_PARAMS UVM_TOOLS_EVENT_QUEUE_DISABLE_EVENTS_PARAMS;

typedef struct {
	NvU64		counterTypeFlags UVM_ALIGN8;	/* IN */
	NV_STATUS	rmStatus;			/* OUT */
} UVM_TOOLS_ENABLE_COUNTERS_PARAMS;

typedef UVM_TOOLS_ENABLE_COUNTERS_PARAMS UVM_TOOLS_DISABLE_COUNTERS_PARAMS;

/* 7.2 queue control block (528 bytes, 8-aligned) */
#define	UVM_TOOLS_EVENT_TYPE_COUNT	64
typedef struct {
	NvU32		get_ahead;
	NvU32		get_behind;
	NvU32		put_ahead;
	NvU32		put_behind;
	NvU64		dropped[UVM_TOOLS_EVENT_TYPE_COUNT] UVM_ALIGN8;
} UvmToolsEventControlData;

/* 7.3 event records: V1 entries are 64 bytes, V2 entries 72 */
enum UvmEventType {
	UvmEventTypeCpuFault = 1,
	UvmEventTypeMigration = 2,
	UvmEventTypeGpuFault = 3,
	UvmEventTypeGpuFaultReplay = 4,
	UvmEventTypeFaultBufferOverflow = 5,
	UvmEventTypeFatalFault = 6,
	UvmEventTypeReadDuplicate = 7,
	UvmEventTypeReadDuplicateInvalidate = 8,
	UvmEventTypePageSizeChange = 9,
	UvmEventTypeThrashingDetected = 10,
	UvmEventTypeThrottlingStart = 11,
	UvmEventTypeThrottlingEnd = 12,
	UvmEventTypeMapRemote = 13,
	UvmEventTypeEviction = 14,
	UvmEventTypeTestHmmSplitInvalidate = 62,
	UvmEventTypeTestAccessCounter = 63
};
#define	UVM_TOOLS_EVENT_TYPES_ALL_MASK	0x7ffeULL	/* bits 1..14 */

typedef struct {
	NvU8	eventType;
	NvU8	accessType;
	NvU8	pad0[2];
	NvU32	cpuId;
	NvU64	address UVM_ALIGN8;
	NvU64	timeStamp UVM_ALIGN8;
	NvU32	pid;
	NvU32	threadId;
	NvU64	pc UVM_ALIGN8;
} UvmEventCpuFaultInfo;

typedef struct {
	NvU8	eventType;
	NvU8	faultType;
	NvU8	accessType;
	NvU8	processorIndex;
	NvU8	reason;
	NvU8	pad0[3];
	NvU64	address UVM_ALIGN8;
	NvU64	timeStamp UVM_ALIGN8;
} UvmEventFatalFaultInfo;

typedef struct {
	NvU8	eventType;
	NvU8	faultType;
	NvU8	reason;
	NvU8	pad0;
	NvU16	processorIndex;
	NvU8	pad1[2];
	NvU64	address UVM_ALIGN8;
	NvU64	timeStamp UVM_ALIGN8;
} UvmEventFatalFaultInfo_V2;

typedef union {
	NvU8			eventType;
	UvmEventCpuFaultInfo	cpuFault;
	UvmEventFatalFaultInfo	fatalFault;
	NvU8			raw[64];
} UvmEventEntry;

typedef union {
	NvU8			eventType;
	UvmEventCpuFaultInfo	cpuFault;
	UvmEventFatalFaultInfo_V2 fatalFault;
	NvU8			raw[72];
} UvmEventEntry_V2;

/* 7.4 counters */
#define	UVM_TOOLS_COUNTER_COUNT		10

/* 7.5 enumerations shared by events */
enum UvmEventFaultType {
	UvmFaultTypeInvalid = 0, UvmFaultTypeInvalidPde = 1, UvmFaultTypeInvalidPte = 2,
	UvmFaultTypeWrite = 3, UvmFaultTypeAtomic = 4, UvmFaultTypeInvalidPdeSize = 5,
	UvmFaultTypeLimitViolation = 6, UvmFaultTypeUnboundInstBlock = 7,
	UvmFaultTypePrivViolation = 8, UvmFaultTypePitchMaskViolation = 9,
	UvmFaultTypeWorkCreation = 10, UvmFaultTypeUnsupportedAperture = 11,
	UvmFaultTypeCompressionFailure = 12, UvmFaultTypeUnsupportedKind = 13,
	UvmFaultTypeRegionViolation = 14, UvmFaultTypePoison = 15, UvmFaultTypeCcViolation = 16
};
enum UvmEventMemoryAccessType {
	UvmEventMemoryAccessTypeInvalid = 0, UvmEventMemoryAccessTypeRead = 1,
	UvmEventMemoryAccessTypeWrite = 2, UvmEventMemoryAccessTypeAtomic = 3,
	UvmEventMemoryAccessTypePrefetch = 4
};
enum UvmEventFatalReason {
	UvmEventFatalReasonInvalidAddress = 1, UvmEventFatalReasonInvalidPermissions = 2,
	UvmEventFatalReasonInvalidFaultType = 3, UvmEventFatalReasonOutOfMemory = 4,
	UvmEventFatalReasonInternalError = 5, UvmEventFatalReasonInvalidOperation = 6
};
enum UvmEventFaultClientType {
	UvmEventFaultClientTypeGpc = 1, UvmEventFaultClientTypeHub = 2
};

#endif /* _NVIDIA_UVM_IOCTL_H_ */
