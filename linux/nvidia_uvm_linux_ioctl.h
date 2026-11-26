/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * nvidia_uvm_linux_ioctl.h - the Linux request numbers of the UVM
 * interface, and their translation into the driver's own numbers.
 *
 * These numbers are the ones a Linux binary issues (a third-party
 * libcuda).  They are not the driver's: nvidia-uvm.ko is the native
 * FreeBSD driver and answers the request numbers of nvidia_uvm_ioctl.h.
 * The optional bridge module nvidia_uvm_linux.ko owns the Linux numbers
 * and translates them (Track #2444).
 *
 * The mapping between the two numbering schemes is ONE table: the list of
 * requests in uvm_linux_request_table.h (written by
 * tools/gen-uvm-request-table.sh from NVIDIA's MIT uvm_ioctl.h), which
 * gives every request its name, the driver's own request id, the Linux
 * request number and the size of the parameter structure the driver copies
 * for it.  The two translations below are small lookups over that table,
 * and the userland unit test (tests/unit/linux_bridge_test.c) walks the
 * same table: a request that is added, renumbered or dropped is changed in
 * one place only.
 */

#ifndef _NVIDIA_UVM_LINUX_IOCTL_H_
#define _NVIDIA_UVM_LINUX_IOCTL_H_

#ifndef _KERNEL
#include <stdint.h>
#endif

#include "nvidia_uvm_ioctl.h"
#include "uvm_linux_request_table.h"

/*
 * The Linux request numbers, compared in full 32 bits.  The two
 * initialisation requests carry a group byte of their own (0x30); the rest
 * are small plain numbers.  They are the numbers uvm-spec r1,
 * 01-user-abi.md section 2.1 gives, and
 * tools/gen-uvm-request-table.sh checks them against NVIDIA's own header
 * when it is given one.
 */
#define	LINUX_UVM_INITIALIZE				0x30000001u
#define	LINUX_UVM_DEINITIALIZE			0x30000002u
#define	LINUX_UVM_CREATE_RANGE_GROUP			23
#define	LINUX_UVM_DESTROY_RANGE_GROUP			24
#define	LINUX_UVM_REGISTER_GPU_VASPACE		25
#define	LINUX_UVM_UNREGISTER_GPU_VASPACE		26
#define	LINUX_UVM_REGISTER_CHANNEL			27
#define	LINUX_UVM_UNREGISTER_CHANNEL			28
#define	LINUX_UVM_ENABLE_PEER_ACCESS			29
#define	LINUX_UVM_DISABLE_PEER_ACCESS			30
#define	LINUX_UVM_SET_RANGE_GROUP			31
#define	LINUX_UVM_MAP_EXTERNAL_ALLOCATION		33
#define	LINUX_UVM_FREE				34
#define	LINUX_UVM_REGISTER_GPU			37
#define	LINUX_UVM_UNREGISTER_GPU			38
#define	LINUX_UVM_PAGEABLE_MEM_ACCESS			39
#define	LINUX_UVM_PREVENT_MIGRATION_RANGE_GROUPS	40
#define	LINUX_UVM_ALLOW_MIGRATION_RANGE_GROUPS		41
#define	LINUX_UVM_SET_PREFERRED_LOCATION		42
#define	LINUX_UVM_UNSET_PREFERRED_LOCATION		43
#define	LINUX_UVM_ENABLE_READ_DUPLICATION		44
#define	LINUX_UVM_DISABLE_READ_DUPLICATION		45
#define	LINUX_UVM_SET_ACCESSED_BY			46
#define	LINUX_UVM_UNSET_ACCESSED_BY			47
#define	LINUX_UVM_MIGRATE				51
#define	LINUX_UVM_MIGRATE_RANGE_GROUP			53
#define	LINUX_UVM_ENABLE_SYSTEM_WIDE_ATOMICS		54
#define	LINUX_UVM_DISABLE_SYSTEM_WIDE_ATOMICS		55
#define	LINUX_UVM_TOOLS_INIT_EVENT_TRACKER		56
#define	LINUX_UVM_TOOLS_SET_NOTIFICATION_THRESHOLD	57
#define	LINUX_UVM_TOOLS_EVENT_QUEUE_ENABLE_EVENTS	58
#define	LINUX_UVM_TOOLS_EVENT_QUEUE_DISABLE_EVENTS	59
#define	LINUX_UVM_TOOLS_ENABLE_COUNTERS		60
#define	LINUX_UVM_TOOLS_DISABLE_COUNTERS		61
#define	LINUX_UVM_TOOLS_READ_PROCESS_MEMORY		62
#define	LINUX_UVM_TOOLS_WRITE_PROCESS_MEMORY		63
#define	LINUX_UVM_TOOLS_GET_PROCESSOR_UUID_TABLE	64
#define	LINUX_UVM_MAP_DYNAMIC_PARALLELISM_REGION	65
#define	LINUX_UVM_UNMAP_EXTERNAL			66
#define	LINUX_UVM_TOOLS_FLUSH_EVENTS			67
#define	LINUX_UVM_ALLOC_SEMAPHORE_POOL		68
#define	LINUX_UVM_CLEAN_UP_ZOMBIE_RESOURCES		69
#define	LINUX_UVM_PAGEABLE_MEM_ACCESS_ON_GPU		70
#define	LINUX_UVM_POPULATE_PAGEABLE			71
#define	LINUX_UVM_VALIDATE_VA_RANGE			72
#define	LINUX_UVM_CREATE_EXTERNAL_RANGE		73
#define	LINUX_UVM_MAP_EXTERNAL_SPARSE			74
#define	LINUX_UVM_MM_INITIALIZE			75
#define	LINUX_UVM_TOOLS_INIT_EVENT_TRACKER_V2		76
#define	LINUX_UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_V2	77
#define	LINUX_UVM_ALLOC_DEVICE_P2P			78
#define	LINUX_UVM_CLEAR_ALL_ACCESS_COUNTERS		79
#define	LINUX_UVM_DISCARD				80
#define	LINUX_UVM_IS_8_SUPPORTED			2047

/*
 * The range of request numbers the Linuxulator hands to a registered
 * handler: it chooses by the low 16 bits of the request word, so the
 * handler sees every request whose low 16 bits fall in [1, 2047].  The two
 * initialisation requests (0x30000001, 0x30000002) have the low bits 1
 * and 2 and are therefore in the range as well; the handler compares the
 * full 32-bit value and translates it.
 */
#define	LINUX_UVM_IOCTL_MIN	1
#define	LINUX_UVM_IOCTL_MAX	2047

/*
 * struct uvm_linux_request - one row of the one table.
 *
 * name is the name of the request, native is the driver's request number,
 * linux is the Linux request number, and arg_size is the size of the
 * parameter structure the driver copies for the request.
 */
struct uvm_linux_request {
	const char	*name;
	uint32_t	native;
	uint32_t	linux;
	size_t		arg_size;
};

/*
 * The rows of the table.  The name of a request is UVM_FOO for the
 * driver's request id UVM_REQ_FOO, the Linux request number
 * LINUX_UVM_FOO and the parameter structure UVM_FOO_PARAMS, so that a
 * row names its three constants and cannot be given one of them by
 * mistake.
 */
#define	UVM_LINUX_REQUEST_ROW(suffix)					\
	{ "UVM_" #suffix, UVM_REQ_ ## suffix, LINUX_UVM_ ## suffix,	\
	  sizeof(UVM_ ## suffix ## _PARAMS) },
#define	UVM_LINUX_REQUEST_ROW0(suffix)					\
	{ "UVM_" #suffix, UVM_REQ_ ## suffix, LINUX_UVM_ ## suffix, 0 },

static const struct uvm_linux_request uvm_linux_requests[] = {
	UVM_LINUX_REQUEST_TABLE(UVM_LINUX_REQUEST_ROW, UVM_LINUX_REQUEST_ROW0)
};

#undef	UVM_LINUX_REQUEST_ROW
#undef	UVM_LINUX_REQUEST_ROW0

#define	UVM_LINUX_REQUEST_COUNT	\
	(sizeof(uvm_linux_requests) / sizeof(uvm_linux_requests[0]))

/*
 * uvm_linux_request_id - the driver's request number for a Linux request
 * number, or 0 when the driver has no request for it.
 *
 * A Linux request number the driver does not know is answered by the
 * driver itself, as an unknown request: the bridge sends UVM_REQ_NONE,
 * and the driver answers ENOSYS on the main node and EINVAL on the tools
 * node, exactly as it does for an unknown native request number.
 *
 * Time:  O(n) in the number of requests.  Space: O(1).
 */
static inline uint32_t
uvm_linux_request_id(uint32_t linux_request)
{
	size_t i;

	for (i = 0; i < UVM_LINUX_REQUEST_COUNT; i++)
		if (uvm_linux_requests[i].linux == linux_request)
			return (uvm_linux_requests[i].native);
	return (0);
}

/*
 * uvm_linux_request_number - the Linux request number of a driver request
 * id, or 0 when the driver has no request with that id.
 *
 * The other direction of the same table: uvm_linux_request_id() translates
 * a Linux request number into the driver's, this one translates the driver's
 * into the Linux one.  The two are inverses for every request the driver
 * has, which the userland unit test checks for each row.
 *
 * A test harness built for Linux needs this one: natively it speaks the
 * driver's own request numbers, and built for Linux it speaks the Linux
 * numbers, which reach the driver through the optional bridge.
 *
 * Time:  O(n) in the number of requests.  Space: O(1).
 */
static inline uint32_t
uvm_linux_request_number(uint32_t request_id)
{
	size_t i;

	for (i = 0; i < UVM_LINUX_REQUEST_COUNT; i++)
		if (uvm_linux_requests[i].native == request_id)
			return (uvm_linux_requests[i].linux);
	return (0);
}

#endif /* _NVIDIA_UVM_LINUX_IOCTL_H_ */
