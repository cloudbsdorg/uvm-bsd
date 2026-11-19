/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * core_group_test.c - the neutral range groups of
 * sys/core/uvm_core_group.h behave as the core/backend split of
 * docs/ARCHITECTURE.md needs it to: the ids of a set start at 1 and only go
 * up, the id 0 means "no group" and is not an error, an id that was never
 * given is not found, and a group that is taken out of its set is not found
 * afterwards.
 *
 * A group set owns no memory: the caller allocates every group it puts in
 * the set, and the set keeps the list and the next id.
 *
 * Red first: compiled before the header existed.
 */

#include <stdint.h>
#include <stdio.h>

#include "core/uvm_core_group.h"

static int fails;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

/* #lizard forgives -- a list of independent checks, one branch each */
int
main(void)
{
	struct uvm_core_group_set set;
	struct uvm_core_group g1 = { 0 }, g2 = { 0 };
	struct uvm_core_group *found;

	/* ---- an empty set ---- */
	uvm_core_group_set_init(&set);
	CHECK(set.next_id == 1, "the first id of a set is 1");

	/* ---- create: the ids start at 1 and go up ---- */
	CHECK(uvm_core_group_create(&set, &g1) == 0, "the first group is created");
	CHECK(g1.id == 1, "the first group has the id 1");
	CHECK(uvm_core_group_create(&set, &g2) == 0, "the second group is created");
	CHECK(g2.id == 2, "the second group has the id 2");
	CHECK(set.next_id == 3, "the ids of a set go up");

	/* ---- lookup ---- */
	CHECK(uvm_core_group_lookup(&set, 1, &found) == 0 && found == &g1,
	    "the first group is found by its id");
	CHECK(uvm_core_group_lookup(&set, 2, &found) == 0 && found == &g2,
	    "the second group is found by its id");
	CHECK(uvm_core_group_lookup(&set, 0, &found) == 0 && found == NULL,
	    "the id 0 is no group and is not an error");
	CHECK(uvm_core_group_lookup(&set, 3, &found) == ENOENT,
	    "an id that was never given is not found");
	CHECK(uvm_core_group_lookup(&set, 3, NULL) == ENOENT,
	    "an id that was never given is not found without an out pointer");

	/* ---- remove ---- */
	CHECK(uvm_core_group_remove(&set, &g1) == 0, "the first group is removed");
	CHECK(uvm_core_group_lookup(&set, 1, &found) == ENOENT,
	    "a removed group is not found any more");
	CHECK(uvm_core_group_lookup(&set, 2, &found) == 0 && found == &g2,
	    "the other group of the set is still there");
	CHECK(uvm_core_group_remove(&set, &g1) == ENOENT,
	    "a group that is not in the set cannot be removed from it");

	/* ---- the group keeps what the caller put in it ---- */
	g2.migratable = false;
	CHECK(uvm_core_group_lookup(&set, 2, &found) == 0 && found != NULL &&
	    !found->migratable, "a group keeps its own flag");

	if (fails != 0) {
		printf("%d check(s) failed\n", fails);
		return (1);
	}
	printf("core_group_test: ok\n");
	return (0);
}
