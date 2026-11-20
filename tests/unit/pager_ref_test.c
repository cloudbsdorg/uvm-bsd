/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pager_ref_test.c - who owns the semaphore pool memory reference of a
 * device pager object (sys/uvm_mmap.c, Track #2272), by exhaustive
 * interleaving of two concurrent mmap calls and one munmap of an existing
 * mapping.
 *
 * Model of cdev_pager_allocate(9): one atomic step that finds the object
 * with the handle (taking a reference on it) or creates it with one
 * reference and runs the constructor.  The mapper holds a memory reference
 * from before the call.  The two protocols:
 *
 *   OLD  after the call, drop the mapper's memory reference when
 *        obj->ref_count > 1 ("the object existed")
 *   NEW  the constructor takes the object's memory reference; the mapper
 *        always drops its own after the call
 *
 * Invariant: the memory is never freed (references 0) while an object or
 * the range still uses it, and everything is released exactly once at the
 * end.  The OLD protocol must fail the invariant in some interleaving (two
 * mappers both seeing a count above one, or a mapper seeing one after a
 * concurrent munmap), the NEW one in none.
 */

#include <stdio.h>
#include <string.h>

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

enum proto { OLD, NEW };

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
mapper_step(struct st *s, int m, enum proto p)
{
	switch (s->pc[m]++) {
	case 0:		/* uvm_sema_mem_ref(r->u.sema.mem) under the VA space lock */
		s->mem_refs++;
		return (1);
	case 1:		/* cdev_pager_allocate: atomic */
		if (s->obj_refs == 0) {
			s->obj_refs = 1;
			if (p == NEW) {
				s->mem_refs++;	/* constructor */
				s->obj_mem_ref = 1;
			} else {
				s->obj_mem_ref = 0;	/* OLD: the mapper's reference is meant to become it */
			}
		} else {
			s->obj_refs++;
		}
		s->a_seen[m] = s->obj_refs;
		return (1);
	case 2:		/* after the call */
		if (p == OLD) {
			if (s->obj_refs > 1)
				mem_unref(s);	/* "the object existed": read obj->ref_count now */
		} else {
			mem_unref(s);
		}
		return (1);
	default:
		return (0);
	}
}

/* U: munmap of the pre-existing mapping: drop one object reference; the last runs the destructor */
static int
unmapper_step(struct st *s, enum proto p)
{
	if (s->pc[2]++ != 0)
		return (0);
	if (--s->obj_refs == 0) {
		mem_unref(s);	/* destructor: the object's one reference (both protocols) */
		(void)p;
	}
	return (1);
}

static int
explore(enum proto p)
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
				(void)unmapper_step(&s, p);
			else
				(void)mapper_step(&s, seq[k], p);
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
		/* the pre-existing object was created with a constructor reference in NEW and the range-time one in OLD: both hold 2 */
		if (s.uaf || s.leak_or_under || s.mem_refs != 0)
			bad++;
	}
	return (bad);
}

int
main(void)
{
	int old_bad = explore(OLD), new_bad = explore(NEW);

	CHECK(old_bad > 0, "the reference-count heuristic must fail in some interleaving (red), failed in %d", old_bad);
	CHECK(new_bad == 0, "the constructor-owned reference failed in %d interleavings", new_bad);
	printf("pager_ref_test: old protocol bad in %d interleavings, new in %d\n", old_bad, new_bad);
	return (fails != 0);
}
