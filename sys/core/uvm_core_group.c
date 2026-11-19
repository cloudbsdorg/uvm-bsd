/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_core_group.c - the neutral range groups of the UVM core.
 *
 * A set of groups is a list and a counter: the counter gives every group
 * an id that starts at 1 and only goes up, and the id 0 is the "no group"
 * that every set answers for.  The set owns no memory and takes no lock;
 * the caller owns the groups and the locking.
 */

#include "uvm_core_group.h"

/*
 * uvm_core_group_set_init - an empty set, whose first id is 1.
 * Time: O(1).  Space: O(1).
 */
void
uvm_core_group_set_init(struct uvm_core_group_set *set)
{
	LIST_INIT(&set->list);
	set->next_id = 1;
}

/*
 * uvm_core_group_create - put `group` in the set, which gives it the next
 * id.  The group is the caller's own memory; the set only keeps it.
 * Time: O(1).  Space: O(1).
 */
int
uvm_core_group_create(struct uvm_core_group_set *set, struct uvm_core_group *group)
{
	group->id = set->next_id++;
	LIST_INSERT_HEAD(&set->list, group, link);
	return (0);
}

/*
 * uvm_core_group_lookup - the group of the set that has the id `id`.  The
 * id 0 means "no group" and is not an error.  0 and the group in `out`
 * when the group is there, ENOENT when it is not; `out` may be NULL.
 * Time: O(n).  Space: O(1).
 */
int
uvm_core_group_lookup(struct uvm_core_group_set *set, uint64_t id, struct uvm_core_group **out)
{
	struct uvm_core_group *g;

	if (out != NULL)
		*out = NULL;
	if (id == 0)
		return (0);	/* "no group" is always valid */
	LIST_FOREACH(g, &set->list, link)
		if (g->id == id) {
			if (out != NULL)
				*out = g;
			return (0);
		}
	return (ENOENT);
}

/*
 * uvm_core_group_remove - take `group` out of the set.  0 when the group
 * was in the set, ENOENT when it was not.  The group itself is left alone;
 * the caller owns its memory.
 * Time: O(n).  Space: O(1).
 */
int
uvm_core_group_remove(struct uvm_core_group_set *set, struct uvm_core_group *group)
{
	struct uvm_core_group *g;

	LIST_FOREACH(g, &set->list, link)
		if (g == group) {
			LIST_REMOVE(g, link);
			return (0);
		}
	return (ENOENT);
}
