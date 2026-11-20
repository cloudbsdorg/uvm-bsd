/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * unload_state_test.c - the unload gate (sys/uvm_unload_state.h, Track
 * #2272): every row of the two predicates, and an exhaustive run of event
 * sequences against an oracle that does not use them: after the module has
 * gone, no open file, VA space or pager object may exist at any later step.
 * An open is two steps, the admission check and the count, which the kernel
 * makes one under uvm.lock (a "locked" run forbids an unload between them;
 * an unlocked run shows why it must).  The sequences go on after the unload,
 * so an open that is admitted late is seen.  Four rules are run: the pre-fix
 * one and an ungated open must be shown wrong, the real pair must be clean.
 */

#include <stdio.h>

#include "uvm_unload_state.h"

static int fails;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

struct world {
	unsigned open_fds;	/* open files, with or without a VA space */
	unsigned pending;	/* opens admitted but not yet counted */
	unsigned va_spaces;
	unsigned sema_objs;	/* semaphore pool pager objects */
	unsigned managed_objs;	/* managed range pager objects */
	bool unloading;
	bool unloaded;
};

enum ev { EV_ADMIT, EV_COUNT, EV_INIT_VAS, EV_CLOSE, EV_MAP_SEMA, EV_MAP_MANAGED, EV_UNMAP_SEMA, EV_UNMAP_MANAGED, EV_UNLOAD, EV_N };

struct rule {
	bool (*busy)(const struct world *);
	bool gated_open;	/* uvm_open consults uvm_open_admit */
	bool locked;		/* admit and count are one step against an unload */
};

/* the pre-fix rule of uvm_unload: VA spaces and managed objects, nothing else */
static bool
old_busy(const struct world *w)
{
	return (w->va_spaces != 0 || w->managed_objs != 0);
}

/* the real predicate, fed the way uvm_in_use_locked feeds it (a pending open is not counted yet) */
static bool
new_busy(const struct world *w)
{
	return (uvm_unload_busy(w->open_fds, w->sema_objs + w->managed_objs, w->va_spaces, 0));
}

/* the pager events: a map or unmap of a pool or managed range */
static void
step_pager(struct world *w, enum ev e)
{
	switch (e) {
	case EV_MAP_SEMA:
		if (w->va_spaces != 0)
			w->sema_objs++;
		return;
	case EV_MAP_MANAGED:
		if (w->va_spaces != 0)
			w->managed_objs++;
		return;
	case EV_UNMAP_SEMA:
		if (w->sema_objs != 0)
			w->sema_objs--;
		return;
	case EV_UNMAP_MANAGED:
		if (w->managed_objs != 0)
			w->managed_objs--;
		return;
	default:
		return;
	}
}

/* the unload: decided by the rule under test */
static void
step_unload(struct world *w, const struct rule *r)
{
	if (r->locked && w->pending != 0)
		return;		/* the open holds uvm.lock between its check and its count */
	if (r->busy(w))
		return;		/* refused: nothing changes */
	w->unloading = true;
	w->unloaded = true;
}

/* apply one event; a step that is not possible in this state changes nothing */
static void
step(struct world *w, enum ev e, const struct rule *r)
{
	switch (e) {
	case EV_ADMIT:
		if (!r->gated_open || uvm_open_admit(w->unloading))
			w->pending++;
		return;
	case EV_COUNT:
		if (w->pending != 0) {
			w->pending--;
			w->open_fds++;
		}
		return;
	case EV_INIT_VAS:
		if (w->va_spaces < w->open_fds)
			w->va_spaces++;
		return;
	case EV_CLOSE:
		if (w->open_fds == 0)
			return;
		w->open_fds--;
		if (w->va_spaces > w->open_fds)
			w->va_spaces = w->open_fds;
		return;
	case EV_UNLOAD:
		step_unload(w, r);
		return;
	default:
		step_pager(w, e);
		return;
	}
}

/* the oracle: once the module is gone, nothing that runs module code may exist */
static bool
module_code_lives(const struct world *w)
{
	return (w->open_fds != 0 || w->pending != 0 || w->sema_objs != 0 || w->managed_objs != 0 ||
	    w->va_spaces != 0);
}

/* run every sequence of `len` events (after the unload too); count the ones where module code lived on */
static unsigned
run(const struct rule *r, unsigned len)
{
	unsigned total = 1, bad = 0, i, n, seq, x;
	struct world w;

	for (i = 0; i < len; i++)
		total *= EV_N;
	for (seq = 0; seq < total; seq++) {
		w = (struct world){ 0 };
		x = seq;
		for (n = 0; n < len; n++) {
			step(&w, (enum ev)(x % EV_N), r);
			x /= EV_N;
			if (w.unloaded && module_code_lives(&w)) {
				bad++;
				break;
			}
		}
	}
	return (bad);
}

int
main(void)
{
	const struct rule pre_fix = { old_busy, false, true };
	const struct rule ungated = { new_busy, false, true };
	const struct rule unlocked = { new_busy, true, false };
	const struct rule real = { new_busy, true, true };

	/* the two predicates, every input alone and together */
	CHECK(!uvm_unload_busy(0, 0, 0, 0), "an idle module may unload");
	CHECK(uvm_unload_busy(1, 0, 0, 0), "an open file without a VA space keeps the module");
	CHECK(uvm_unload_busy(0, 1, 0, 0), "a pager object that outlived its fd keeps the module");
	CHECK(uvm_unload_busy(0, 0, 1, 0), "a VA space keeps the module");
	CHECK(uvm_unload_busy(0, 0, 0, 1), "a registered GPU keeps the module");
	CHECK(uvm_unload_busy(3, 2, 1, 1), "everything busy is busy");
	CHECK(uvm_open_admit(false), "an open is admitted before the unload is decided");
	CHECK(!uvm_open_admit(true), "an open is refused once the unload is decided");

	/* each broken rule is shown wrong, the real one is clean */
	CHECK(run(&pre_fix, 6) > 0, "the pre-fix rule must be shown wrong by the oracle");
	CHECK(run(&ungated, 6) > 0, "an open that does not consult uvm_open_admit must be shown wrong");
	CHECK(run(&unlocked, 6) > 0, "admit and count without one lock step must be shown wrong");
	CHECK(run(&real, 6) == 0, "the gated, locked rule let module code live past the unload");

	if (fails == 0)
		printf("unload_state_test: all rows pass\n");
	return (fails != 0);
}
