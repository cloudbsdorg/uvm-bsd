/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_core_group.h - the neutral range groups of the UVM core: the groups
 * a process's address space is split into, each with an id of its own and
 * a flag that says whether the ranges in it may be migrated.
 *
 * A group set holds the groups of one address space.  It owns no memory:
 * the caller allocates every group it puts in the set and frees it again
 * when the group is gone.  The set names no device and no vendor, it takes
 * no lock and it allocates nothing; the caller owns the locking.
 *
 * The header is usable from the kernel and from userland (like
 * sys/core/uvm_core_types.h), so that tests/unit/core_group_test.c can
 * compile it outside the kernel.
 */

#ifndef _UVM_CORE_GROUP_H_
#define	_UVM_CORE_GROUP_H_

#include <sys/types.h>
#ifdef _KERNEL
#include <sys/_null.h>
#include <sys/errno.h>
#include <sys/stdint.h>
#else
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#endif
#include <sys/queue.h>

/* ---- one group of an address space ---- */

/*
 * A group.  The id is given by the set the group is in, it starts at 1 and
 * only goes up; the id 0 is the "no group" of every set.  The flag is the
 * group's own business.
 */
struct uvm_core_group {
	LIST_ENTRY(uvm_core_group) link;
	uint64_t	id;
	bool		migratable;
};

/* the groups of one address space */
struct uvm_core_group_set {
	LIST_HEAD(, uvm_core_group) list;
	uint64_t	next_id;
};

/* ---- a set of groups ---- */

/*
 * uvm_core_group_set_init - an empty set, whose first id is 1.
 * Time: O(1).  Space: O(1).
 */
void	uvm_core_group_set_init(struct uvm_core_group_set *set);

/*
 * uvm_core_group_create - put `group` in the set, which gives it the next
 * id.  The group is the caller's own memory; the set only keeps it.
 * Time: O(1).  Space: O(1).
 */
int	uvm_core_group_create(struct uvm_core_group_set *set, struct uvm_core_group *group);

/*
 * uvm_core_group_lookup - the group of the set that has the id `id`.  The
 * id 0 means "no group" and is not an error.  0 and the group in `out`
 * when the group is there, ENOENT when it is not; `out` may be NULL.
 * Time: O(n).  Space: O(1).
 */
int	uvm_core_group_lookup(struct uvm_core_group_set *set, uint64_t id,
    struct uvm_core_group **out);

/*
 * uvm_core_group_remove - take `group` out of the set.  0 when the group
 * was in the set, ENOENT when it was not.  The group itself is left alone;
 * the caller owns its memory.
 * Time: O(n).  Space: O(1).
 */
int	uvm_core_group_remove(struct uvm_core_group_set *set, struct uvm_core_group *group);

#endif /* _UVM_CORE_GROUP_H_ */
