/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_core_device.h - the device registry of the UVM core: the backends
 * that are registered with it, the devices they brought, and the way a
 * device is found by an identity.
 *
 * The registry is the core's own bookkeeping.  It names no device vendor:
 * a backend registers itself with a struct uvm_core_backend and is asked
 * to take an identity that the core does not understand; the backend that
 * takes it fills the device and the core gives the device an id out of
 * 1..256.  A device is found again by the same identity, by its id, or
 * by a walk over all of them.
 *
 * The reference count of a device is the core's own: the probe takes the
 * first one, every further uvm_core_device_probe of the same identity and
 * every uvm_core_device_retain takes one more, and uvm_core_device_release
 * gives one back.  When the last one goes the device's release operation
 * is called, the backend is told that the device is gone, and the id is
 * free again.
 *
 * The header is usable from the kernel and from userland (like
 * sys/core/uvm_core_types.h), so that tests/unit/core_device_test.c can
 * compile it outside the kernel.
 */

#ifndef _UVM_CORE_DEVICE_H_
#define	_UVM_CORE_DEVICE_H_

#include <sys/types.h>

#include "uvm_core_ops.h"

/*
 * The ids the core hands out are the processor ids of the devices:
 * 1..256.  The id 0 is the CPU and is never a device.
 */
#define	UVM_CORE_DEVICE_ID_FIRST	UVM_CORE_ID_FIRST
#define	UVM_CORE_DEVICE_ID_LAST	UVM_CORE_ID_LAST

/*
 * The bytes of an identity the core keeps.  A longer identity is cut
 * short; two identities that share their first 32 bytes are one device.
 */
#define	UVM_CORE_IDENTITY_MAX	32

/* the backends the core holds at once */
#define	UVM_CORE_BACKEND_MAX	8

/*
 * uvm_core_registry_init - the registry is ready for the backends.
 * The module calls it when it loads.
 * Time: O(1).  Space: O(1).
 */
void	uvm_core_registry_init(void);

/*
 * uvm_core_registry_deinit - the registry is empty and its lock is gone.
 * The module calls it when it unloads, when no device is left.
 * Time: O(n) in the devices that are still there, one for each.
 * Space: O(1).
 */
void	uvm_core_registry_deinit(void);

/*
 * uvm_core_device_probe - the device an identity names.  The backends are
 * asked in the order they registered themselves, and the one that takes
 * the identity fills the device; the device's describe operation is
 * called and its answer is kept in the device's capabilities.  A device
 * that is already there for the same identity is given one more reference
 * and asked no backend again.  0 and the device when there is one,
 * ENODEV when no backend takes the identity, ENOSPC when no id is free
 * and ENOMEM when the device cannot be allocated.
 * Time: O(n) in the devices and in the backends, one for each.
 * Space: O(1).
 */
int	uvm_core_device_probe(const void *identity, size_t identity_len,
	    struct uvm_core_device **out);

/*
 * uvm_core_device_lookup - the device with the id `id`, or NULL when
 * there is none.  The id 0 is the CPU and is no device.
 * Time: O(1).  Space: O(1).
 */
struct uvm_core_device *uvm_core_device_lookup(uvm_core_id_t id);

/*
 * uvm_core_device_retain - take a reference on the device, so that it
 * outlives what holds it now.
 * Time: O(1).  Space: O(1).
 */
void	uvm_core_device_retain(struct uvm_core_device *dev);

/*
 * uvm_core_device_release - give back a reference.  When the last one
 * goes the device's release operation is called, the backend is told that
 * the device is gone, and the id is free again.
 * Time: O(1).  Space: O(1).
 */
void	uvm_core_device_release(struct uvm_core_device *dev);

/*
 * uvm_core_device_foreach - walk over the devices and call `fn` on each
 * of them.  The walk stops at the first answer that is not 0 and gives
 * that answer back; 0 when every device was walked.
 * Time: O(n) in the devices.  Space: O(1).
 */
int	uvm_core_device_foreach(int (*fn)(struct uvm_core_device *, void *), void *arg);

/*
 * uvm_core_backend_unregister_checked - the backend leaves the core.
 * EBUSY when devices of that backend are still there, ENOENT when the
 * backend was not registered, 0 when it left.  The unregister of
 * sys/core/uvm_core_ops.h calls this and does not report the answer.
 * Time: O(n) in the devices.  Space: O(1).
 */
int	uvm_core_backend_unregister_checked(struct uvm_core_backend *backend);

#endif /* _UVM_CORE_DEVICE_H_ */
