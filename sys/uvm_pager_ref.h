/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_pager_ref.h - who owns the memory reference of a semaphore pool's
 * device pager object (Track #2272), as pure functions so that userland
 * tests check the rule the kernel code uses.
 *
 * A mapper takes a memory reference before cdev_pager_allocate(9), which
 * either creates the object (and runs the constructor, once, after it has
 * dropped the device pager's list mutex, while a concurrent finder of the
 * same handle waits) or finds an existing one.  The object's own
 * reference is taken by the constructor, so it exists exactly when the
 * object does; the mapper then always drops its own, whatever it saw.
 * Deciding from the object's reference count whether "this mapper created
 * it" is a race between two mappers of one pool.
 */

#ifndef UVM_PAGER_REF_H
#define UVM_PAGER_REF_H

#ifndef _KERNEL
#include <stdbool.h>
#endif

/* memory references the constructor of a new pager object takes */
static inline unsigned
uvm_pager_ctor_mem_refs(void)
{
	return (1);
}

/*
 * Whether the mapper drops its own reference once cdev_pager_allocate has
 * returned.  Always: the rule takes no input, so what the mapper saw of the
 * object (its reference count, which another mapper may be changing) can
 * never decide it.
 */
static inline bool
uvm_pager_mapper_drops_ref(void)
{
	return (true);
}

#endif /* UVM_PAGER_REF_H */
