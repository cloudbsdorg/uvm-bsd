/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_core_types.h - the vendor-neutral vocabulary of the UVM core: the
 * processor ids and their mask, the protections, the apertures, the page
 * sizes, the opaque work trackers, the physical runs and the fault
 * events.
 *
 * The core (sys/core/) never names a GPU vendor.  Every type here is
 * defined by value and not by including a backend's header, so that the
 * core and a backend can be compiled apart and a second vendor can be
 * added without touching the core.  The design is ours; see
 * docs/ARCHITECTURE.md.
 *
 * The header is usable from the kernel and from userland (like
 * sys/uvm_pte.h), so that tests/unit/core_types_test.c can compile it
 * outside the kernel.  A kernel build compiles with -nostdinc, so the
 * standard integer and boolean headers are taken in userland only; the
 * kernel's own sys/types.h carries them.
 */

#ifndef _UVM_CORE_TYPES_H_
#define	_UVM_CORE_TYPES_H_

#include <sys/types.h>
#ifndef _KERNEL
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#endif

/* ---- processor ids ---- */

/*
 * A processor id names a device the core drives: 0 is the CPU, 1..256
 * are the devices.  The numbers are the module's own processor ids.
 */
typedef uint16_t uvm_core_id_t;

#define	UVM_CORE_ID_CPU		0
#define	UVM_CORE_ID_FIRST	1
#define	UVM_CORE_ID_LAST	256
#define	UVM_CORE_ID_COUNT	(UVM_CORE_ID_LAST + 1)	/* 257 ids: 0..256 */

/* a bitmap over the 257 processor ids */
struct uvm_core_id_mask {
	uint64_t	bits[(UVM_CORE_ID_COUNT + 63) / 64];
};

/*
 * Set, clear and test one id.  The id is not checked against the
 * bounds: the core only ever passes an id it has already validated.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_id_mask_set(struct uvm_core_id_mask *m, uvm_core_id_t id)
{
	m->bits[id / 64] |= 1ULL << (id % 64);
}

static inline void
uvm_core_id_mask_clear(struct uvm_core_id_mask *m, uvm_core_id_t id)
{
	m->bits[id / 64] &= ~(1ULL << (id % 64));
}

static inline bool
uvm_core_id_mask_test(const struct uvm_core_id_mask *m, uvm_core_id_t id)
{
	return ((m->bits[id / 64] & (1ULL << (id % 64))) != 0);
}

/* ---- protections ---- */

/*
 * The protection of a mapping.  The values are the ones the module's own
 * protection type carries, so that the two can be converted by value.
 */
typedef enum {
	UVM_CORE_PROT_NONE = 0,
	UVM_CORE_PROT_READ_ONLY = 1,
	UVM_CORE_PROT_READ_WRITE = 2,
	UVM_CORE_PROT_READ_WRITE_ATOMIC = 3
} uvm_core_prot_t;

/* ---- apertures ---- */

/*
 * Where a physical address lives, as the core sees it: in the device
 * itself, in system memory, or in a peer device.  The peers are numbered
 * in the order the devices were registered.
 */
typedef enum {
	UVM_CORE_APERTURE_LOCAL = 0,
	UVM_CORE_APERTURE_SYSTEM = 1,
	UVM_CORE_APERTURE_PEER0 = 2,
	UVM_CORE_APERTURE_PEER1 = 3,
	UVM_CORE_APERTURE_PEER2 = 4,
	UVM_CORE_APERTURE_PEER3 = 5,
	UVM_CORE_APERTURE_PEER4 = 6,
	UVM_CORE_APERTURE_PEER5 = 7,
	UVM_CORE_APERTURE_PEER6 = 8,
	UVM_CORE_APERTURE_PEER7 = 9
} uvm_core_aperture_t;

/* ---- page sizes ---- */

/*
 * The page sizes the core knows, with their sizes in bytes.  A device
 * declares the ones it supports in its capabilities, as a bitmask of the
 * UVM_CORE_PAGE_SIZE_BIT_* values below.
 */
typedef enum {
	UVM_CORE_PAGE_SIZE_4K = 0x1000,
	UVM_CORE_PAGE_SIZE_64K = 0x10000,
	UVM_CORE_PAGE_SIZE_2M = 0x200000,
	UVM_CORE_PAGE_SIZE_512M = 0x20000000
} uvm_core_page_size_t;

#define	UVM_CORE_PAGE_SIZE_BIT_4K	(1u << 0)
#define	UVM_CORE_PAGE_SIZE_BIT_64K	(1u << 1)
#define	UVM_CORE_PAGE_SIZE_BIT_2M	(1u << 2)
#define	UVM_CORE_PAGE_SIZE_BIT_512M	(1u << 3)

/* the bit of a page size in a capabilities mask */
static inline uint32_t
uvm_core_page_size_bit(uvm_core_page_size_t size)
{
	switch (size) {
	case UVM_CORE_PAGE_SIZE_4K:
		return (UVM_CORE_PAGE_SIZE_BIT_4K);
	case UVM_CORE_PAGE_SIZE_64K:
		return (UVM_CORE_PAGE_SIZE_BIT_64K);
	case UVM_CORE_PAGE_SIZE_2M:
		return (UVM_CORE_PAGE_SIZE_BIT_2M);
	case UVM_CORE_PAGE_SIZE_512M:
		return (UVM_CORE_PAGE_SIZE_BIT_512M);
	default:
		return (0);
	}
}

/* whether a capabilities mask declares a page size */
static inline bool
uvm_core_page_size_supported(uint32_t page_sizes, uvm_core_page_size_t size)
{
	return ((uvm_core_page_size_bit(size) & page_sizes) != 0);
}

/* ---- work trackers ---- */

/*
 * A tracker is an opaque 64-bit handle a backend issues for work it has
 * started.  The core only waits on it and orders later work after it; it
 * never looks inside.  The value 0 means there is no tracker.
 */
typedef uint64_t uvm_core_tracker_t;

#define	UVM_CORE_TRACKER_NONE	((uvm_core_tracker_t)0)

/* ---- physical runs ---- */

/*
 * A run of pages in one place: a bus or device physical address, the
 * size of its pages, the aperture it is seen through, and how many pages
 * it holds.
 */
struct uvm_core_phys_run {
	uint64_t		address;
	uvm_core_page_size_t	page_size;
	uvm_core_aperture_t	aperture;
	uint32_t		page_count;
};

/* ---- fault events ---- */

/* what the access that faulted was */
typedef enum {
	UVM_CORE_FAULT_ACCESS_READ = 0,
	UVM_CORE_FAULT_ACCESS_WRITE = 1,
	UVM_CORE_FAULT_ACCESS_ATOMIC = 2,
	UVM_CORE_FAULT_ACCESS_PREFETCH = 3
} uvm_core_fault_access_t;

/* whether the fault can be replayed once it is serviced */
typedef enum {
	UVM_CORE_FAULT_CLASS_REPLAYABLE = 0,
	UVM_CORE_FAULT_CLASS_NON_REPLAYABLE = 1
} uvm_core_fault_class_t;

/*
 * A fault, as the core sees it: which device faulted, in which address
 * space, at which virtual address, what the access was, whether the
 * fault can be replayed, and the backend's own token for the fault.
 * The token is opaque to the core and comes back unchanged in the
 * answer the core sends to the backend.
 *
 * The layout of this structure is part of the contract between the core
 * and a backend; tests/unit/core_types_test.c holds it fixed.
 */
struct uvm_core_fault_event {
	uvm_core_id_t		device_id;
	void			*as;		/* the address space that faulted */
	uint64_t		vaddr;
	uvm_core_fault_access_t	access;
	uvm_core_fault_class_t	fault_class;
	uint64_t		token;		/* the backend's opaque token */
};

#endif /* _UVM_CORE_TYPES_H_ */
