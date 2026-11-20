/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pager_ref_test.c - who owns the semaphore pool memory reference of a
 * device pager object (sys/uvm_pager_ref.h, the rule sys/uvm_mmap.c uses,
 * Track #2272), by exhaustive interleaving of two concurrent mmap calls
 * and one munmap of an existing mapping.
 *
 * Model of cdev_pager_allocate(9): one atomic step that finds the object
 * with the handle (taking a reference on it) or creates it with one
 * reference and runs the constructor.  The mapper holds a memory reference
 * from before the call.  The decisions come from uvm_pager_ref.h: how many
 * references the constructor takes, and whether the mapper drops its own
 * after the call (given the object reference count it saw).
 *
 * Invariant: the memory is never freed (references 0) while an object or
 * the range still uses it, and everything is released exactly once at the
 * end.  The pre-fix rule ("the mapper keeps its reference when it saw a
 * count of one", constructor takes none) fails it; build this file against
 * that header to see the test red.
 */

#include <stdio.h>
#include <string.h>

#include "uvm_pager_ref.h"

static int fails;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

struct st {
	int mem_refs;
	int obj_refs;		/* vm_object ref_count; 0 means no object */
	int obj_mem_ref;	/* the object holds a memory reference */
	int pc[3];		/* program counters of mappers A, B and the unmapper U */
	int a_seen[2];		/* the ref_count each mapper saw after its allocate */
	int uaf;		/* memory freed while still used */
	int leak_or_under;
};

static void
mem_unref(struct st *s)
{
	if (--s->mem_refs < 0)
		s->leak_or_under = 1;
}

/* the memory is used while the range exists (modelled by the final step) or an object holds it */
static void
check_alive(struct st *s)
{
	if (s->obj_refs > 0 && s->mem_refs <= 0)
		s->uaf = 1;
}

/* one step of mapper m (0 or 1); returns 0 when finished */
static int
mapper_step(struct st *s, int m)
{
	switch (s->pc[m]++) {
	case 0:		/* uvm_sema_mem_ref(r->u.sema.mem) under the VA space lock */
		s->mem_refs++;
		return (1);
	case 1:		/* cdev_pager_allocate: atomic */
		if (s->obj_refs == 0) {
			s->obj_refs = 1;
			s->mem_refs += (int)uvm_pager_ctor_mem_refs();	/* constructor */
			s->obj_mem_ref = uvm_pager_ctor_mem_refs() > 0;
		} else {
			s->obj_refs++;
		}
		s->a_seen[m] = s->obj_refs;
		return (1);
	case 2:		/* after the call */
		if (uvm_pager_mapper_drops_ref((unsigned)s->obj_refs))
			mem_unref(s);
		return (1);
	default:
		return (0);
	}
}

/* U: munmap of the pre-existing mapping: drop one object reference; the last runs the destructor */
static int
unmapper_step(struct st *s)
{
	if (s->pc[2]++ != 0)
		return (0);
	if (--s->obj_refs == 0) {
		mem_unref(s);	/* destructor: the object's one reference */
	}
	return (1);
}

static int
explore(void)
{
	int bad = 0, order, n;

	/* orders: interleavings of A(3) B(3) U(1) as a base-3 word of 7 digits with the right multiplicities */
	for (order = 0; order < 2187; order++) {
		struct st s;
		int x = order, cnt[3] = { 0, 0, 0 }, k;
		int seq[7];

		for (k = 0; k < 7; k++) {
			seq[k] = x % 3;
			x /= 3;
			cnt[seq[k]]++;
		}
		if (cnt[0] != 3 || cnt[1] != 3 || cnt[2] != 1)
			continue;
		memset(&s, 0, sizeof(s));
		/* a pre-existing mapping: the range's reference and the object's own */
		s.mem_refs = 2;
		s.obj_refs = 1;
		s.obj_mem_ref = 1;
		for (k = 0; k < 7; k++) {
			if (seq[k] == 2)
				(void)unmapper_step(&s);
			else
				(void)mapper_step(&s, seq[k]);
			check_alive(&s);
		}
		/* everyone unmaps what they mapped, then the range goes (its reference) */
		n = s.obj_refs;
		while (n-- > 0) {
			if (--s.obj_refs == 0)
				mem_unref(&s);	/* destructor */
			check_alive(&s);
		}
		/* the object's pre-existing reference is gone with the object; the range's remains */
		mem_unref(&s);
		if (s.uaf || s.leak_or_under || s.mem_refs != 0)
			bad++;
	}
	return (bad);
}

/* the rule itself, whatever the model: the constructor owns a reference and the mapper's drop is unconditional */
static void
check_rule(void)
{
	unsigned seen;

	CHECK(uvm_pager_ctor_mem_refs() == 1, "the constructor takes the object's one reference");
	for (seen = 0; seen < 8; seen++)
		CHECK(uvm_pager_mapper_drops_ref(seen), "the mapper drops its reference after seeing %u", seen);
}

int
main(void)
{
	int bad = explore();

	check_rule();
	CHECK(bad == 0, "the reference rule failed in %d interleavings", bad);
	printf("pager_ref_test: %d bad interleavings\n", bad);
	return (fails != 0);
}
