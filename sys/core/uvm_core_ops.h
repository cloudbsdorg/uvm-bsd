/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_core_ops.h - the operations table of the UVM core: what the core
 * asks a device to do, and how a vendor backend registers itself.
 *
 * A backend fills in a struct uvm_core_device_ops at probe and hands it
 * to the core with the device.  Everything the core asks of a device
 * goes through that table, so the core never learns what the device
 * really is.  The six groups are the ones of docs/ARCHITECTURE.md:
 * device lifetime, address spaces, work, faults, device memory and the
 * user ABI hooks.
 *
 * Every operation returns an errno (0 on success).  The backend's own
 * ABI layer translates a core result into the status codes its users
 * expect; the core never sees them.
 *
 * Locks: the core holds none of the backend's locks when it calls an
 * operation, and an operation must not call back into the core.  The
 * core's VA space lock may be held for read while an operation runs,
 * and never for write.
 *
 * The design is ours; see docs/ARCHITECTURE.md.
 */

#ifndef _UVM_CORE_OPS_H_
#define	_UVM_CORE_OPS_H_

#include "uvm_core_types.h"

struct uvm_core_device;

/* ---- device capabilities ---- */

/*
 * What a device says it can do.  The core reads them when the device is
 * probed and asks for them again through the describe operation.
 */
struct uvm_core_device_caps {
	char		name[64];		/* the device's name */
	uint64_t	memory_bytes;		/* the device memory it has */
	uint32_t	page_sizes;		/* the page sizes it supports, as bits */
	uint64_t	max_virtual_address;	/* the largest virtual address it maps */
	bool		supports_replayable_faults;
	bool		supports_sparse;
	bool		supports_reflected;
	uint64_t	dma_window_start;	/* the window the core may program */
	uint64_t	dma_window_limit;
};

/* ---- group 1: the lifetime of a device ---- */

/*
 * probe: the core has found a device it does not know yet and hands it
 * the identity the backend chose (an opaque blob of the backend's own
 * making).  The backend matches the identity against the hardware and
 * fills in what it can do.  Called once per device, with no core lock
 * held and with the device's own lock not yet taken.
 */
typedef int (*uvm_core_op_probe)(struct uvm_core_device *dev, const void *identity,
    size_t identity_len);

/*
 * retain: the core takes a reference on the device, because something
 * that outlives the probe now refers to it.  Called under the core's own
 * device-list lock, which the backend must not take.
 */
typedef int (*uvm_core_op_retain)(struct uvm_core_device *dev);

/*
 * release: the core drops a reference taken by retain.  The device is
 * gone when the last reference goes.  Called under the core's own
 * device-list lock.
 */
typedef int (*uvm_core_op_release)(struct uvm_core_device *dev);

/*
 * describe: the device says what it can do, into the capabilities the
 * core hands over.  Called when the device is probed and again whenever
 * the core needs the answer afresh.  No core lock is held.
 */
typedef int (*uvm_core_op_describe)(struct uvm_core_device *dev,
    struct uvm_core_device_caps *caps);

/* ---- group 2: the address spaces of a device ---- */

/*
 * create_va_space: the core binds a device address space to one of its
 * VA spaces and gets the device's own handle for it.  Called when a
 * device is registered with a VA space, with the core's VA space lock
 * held for read.
 */
typedef int (*uvm_core_op_create_va_space)(struct uvm_core_device *dev, void *core_vas,
    void **out_device_as);

/*
 * destroy_va_space: the device gives back the address space the core
 * bound to it.  Called when the VA space or the registration goes away,
 * with the core's VA space lock held for read.
 */
typedef int (*uvm_core_op_destroy_va_space)(struct uvm_core_device *dev, void *device_as);

/*
 * map_runs: the device maps a run of pages at a virtual address, with a
 * protection.  The runs are given in order and cover the mapping; the
 * caching hint is the one the mapping was asked for, which the core
 * passes on unchanged and never interprets, because the encoding of it
 * is the backend's.  Called with the core's VA space lock held for read
 * and with the range tree's own lock held.
 */
typedef int (*uvm_core_op_map_runs)(struct uvm_core_device *dev, void *device_as,
    uint64_t vaddr, const struct uvm_core_phys_run *runs, size_t run_count,
    uvm_core_prot_t prot, uint32_t cache_hint);

/*
 * unmap_runs: the device unmaps a number of pages at a virtual address.
 * Called with the core's VA space lock held for read and with the range
 * tree's own lock held.
 */
typedef int (*uvm_core_op_unmap_runs)(struct uvm_core_device *dev, void *device_as,
    uint64_t vaddr, size_t page_count);

/*
 * map_sparse: the device reserves a number of pages at a virtual
 * address without giving them any physical backing.  Called only when
 * the device's capabilities say that it supports sparse mappings, with
 * the core's VA space lock held for read.
 */
typedef int (*uvm_core_op_map_sparse)(struct uvm_core_device *dev, void *device_as,
    uint64_t vaddr, size_t page_count);

/*
 * map_reflected: the device maps a number of pages that reflect
 * something the core already has, such as a range of system memory.
 * Called only when the device's capabilities say that it supports
 * reflected mappings, with the core's VA space lock held for read.
 */
typedef int (*uvm_core_op_map_reflected)(struct uvm_core_device *dev, void *device_as,
    uint64_t vaddr, size_t page_count, uvm_core_prot_t prot);

/* ---- group 3: the work of a device ---- */

/*
 * work_wait: the core waits until the work the tracker names has
 * finished.  Called with no core lock held; the backend may block.
 */
typedef int (*uvm_core_op_work_wait)(struct uvm_core_device *dev, uvm_core_tracker_t tracker);

/*
 * work_order_after: the device starts work that must run after the work
 * the tracker names, and gives the core a tracker for the new work.
 * Called with no core lock held.
 */
typedef int (*uvm_core_op_work_order_after)(struct uvm_core_device *dev,
    uvm_core_tracker_t tracker, uvm_core_tracker_t *out_tracker);

/*
 * work_copy: the device copies from one set of runs to another, as
 * migration needs, and gives the core a tracker for the copy.  Called
 * with no core lock held.
 */
typedef int (*uvm_core_op_work_copy)(struct uvm_core_device *dev,
    const struct uvm_core_phys_run *src, const struct uvm_core_phys_run *dst,
    size_t run_count, uvm_core_tracker_t *out_tracker);

/* ---- group 4: the faults of a device ---- */

/*
 * fault_service: the core has made the pages of the fault resident and
 * tells the device that the fault is serviced.  The device turns this
 * into whatever its hardware needs.  Called from the core's fault
 * service queue, with no core lock held.
 */
typedef int (*uvm_core_op_fault_service)(struct uvm_core_device *dev,
    const struct uvm_core_fault_event *event);

/*
 * fault_replay: the core tells the device to replay the access that
 * faulted.  Called from the core's fault service queue, with no core
 * lock held.
 */
typedef int (*uvm_core_op_fault_replay)(struct uvm_core_device *dev,
    const struct uvm_core_fault_event *event);

/*
 * fault_cancel: the core tells the device to cancel the access that
 * faulted.  Called from the core's fault service queue, with no core
 * lock held.
 */
typedef int (*uvm_core_op_fault_cancel)(struct uvm_core_device *dev,
    const struct uvm_core_fault_event *event);

/*
 * fault_kill_context: the core tells the device that the context which
 * faulted is to be killed.  Called from the core's fault service queue,
 * with no core lock held.
 */
typedef int (*uvm_core_op_fault_kill_context)(struct uvm_core_device *dev,
    const struct uvm_core_fault_event *event);

/* ---- group 5: the device memory of a device ---- */

/*
 * mem_alloc: the device allocates device pages in its own allocator and
 * the core counts them.  Called when the core needs memory on the
 * device, with no core lock held.
 */
typedef int (*uvm_core_op_mem_alloc)(struct uvm_core_device *dev, size_t bytes,
    uvm_core_page_size_t page_size, void **out_allocation);

/*
 * mem_free: the device gives back an allocation and the core stops
 * counting it.  Called when the core drops the memory, with no core
 * lock held.
 */
typedef int (*uvm_core_op_mem_free)(struct uvm_core_device *dev, void *allocation);

/*
 * mem_evict: the core needs the memory back and asks the device to
 * evict it, so that the device's own accounting follows.  Called from
 * the core's eviction path, with no core lock held.
 */
typedef int (*uvm_core_op_mem_evict)(struct uvm_core_device *dev, void *allocation);

/*
 * mem_resident_bytes: the device says how much of an allocation is
 * resident, so that the core's residency and migration policy has
 * something to work on.  Called with no core lock held.
 */
typedef int (*uvm_core_op_mem_resident_bytes)(struct uvm_core_device *dev, void *allocation,
    uint64_t *out_resident_bytes);

/* ---- group 6: the hooks the user ABI needs ---- */

/*
 * user_abi_va_space_released: the core is giving up a VA space, and the
 * backend's user ABI layer must drop what it kept for it.  Called when
 * the VA space is destroyed, with the core's VA space lock held for
 * read.
 */
typedef int (*uvm_core_op_user_abi_va_space_released)(struct uvm_core_device *dev,
    void *core_vas);

/*
 * user_abi_range_unmapped: the core has unmapped a range, and the
 * backend's user ABI layer must stop reporting it.  Called when the
 * range goes away, with the core's VA space lock held for read.
 */
typedef int (*uvm_core_op_user_abi_range_unmapped)(struct uvm_core_device *dev,
    void *core_vas, uint64_t start, uint64_t end);

/*
 * user_abi_translate_status: the backend's user ABI layer needs a
 * result of the core as a status code of its own, and the mapping from
 * one to the other belongs to the backend.  Called when a core result
 * has to be reported to a user, with no core lock held.
 */
typedef int (*uvm_core_op_user_abi_translate_status)(struct uvm_core_device *dev,
    int core_errno, uint64_t *out_vendor_status);

/* ---- the table ---- */

/*
 * The operations a device offers the core.  The six groups are the ones
 * of docs/ARCHITECTURE.md.  An operation the device does not have is
 * left out; the core checks the device's capabilities before it calls
 * one that is not always there.
 */
struct uvm_core_device_ops {
	/* device lifetime */
	uvm_core_op_probe		*probe;
	uvm_core_op_retain		*retain;
	uvm_core_op_release		*release;
	uvm_core_op_describe		*describe;

	/* address spaces */
	uvm_core_op_create_va_space	*create_va_space;
	uvm_core_op_destroy_va_space	*destroy_va_space;
	uvm_core_op_map_runs		*map_runs;
	uvm_core_op_unmap_runs		*unmap_runs;
	uvm_core_op_map_sparse		*map_sparse;
	uvm_core_op_map_reflected	*map_reflected;

	/* work */
	uvm_core_op_work_wait		*work_wait;
	uvm_core_op_work_order_after	*work_order_after;
	uvm_core_op_work_copy		*work_copy;

	/* faults */
	uvm_core_op_fault_service	*fault_service;
	uvm_core_op_fault_replay	*fault_replay;
	uvm_core_op_fault_cancel	*fault_cancel;
	uvm_core_op_fault_kill_context	*fault_kill_context;

	/* device memory */
	uvm_core_op_mem_alloc		*mem_alloc;
	uvm_core_op_mem_free		*mem_free;
	uvm_core_op_mem_evict		*mem_evict;
	uvm_core_op_mem_resident_bytes	*mem_resident_bytes;

	/* the hooks the user ABI needs */
	uvm_core_op_user_abi_va_space_released	*user_abi_va_space_released;
	uvm_core_op_user_abi_range_unmapped	*user_abi_range_unmapped;
	uvm_core_op_user_abi_translate_status	*user_abi_translate_status;
};

/*
 * A device of the core: an id, what it can do, the operations it offers
 * and whatever the backend keeps of its own behind it.
 */
struct uvm_core_device {
	uvm_core_id_t			id;
	struct uvm_core_device_caps	caps;
	struct uvm_core_device_ops	*ops;
	void				*backend_private;
};

/*
 * A backend: one per vendor, registered when the module loads.  The
 * name is the vendor's own name, for the core's diagnostics.  The probe
 * finds the device an identity names and fills in the device the core
 * asked for; the release lets go of it.
 */
struct uvm_core_backend {
	const char	*name;
	int		(*probe_by_identity)(const void *identity, size_t identity_len,
		    uvm_core_id_t id, struct uvm_core_device *device);
	int		(*release)(struct uvm_core_device *device);
};

/*
 * Register a backend and unregister it.  A backend registers itself when
 * the module loads and unregisters itself when the module unloads; the
 * core keeps the list of them under its own lock.
 */
int	uvm_core_backend_register(struct uvm_core_backend *backend);
void	uvm_core_backend_unregister(struct uvm_core_backend *backend);

#endif /* _UVM_CORE_OPS_H_ */
