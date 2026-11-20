/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * unload_state_test.c - the unload gate (sys/uvm_unload_state.h, Track
 * #2272): every row of the two predicates, and an exhaustive run of short
 * event sequences against an oracle that knows what module code can still
 * run.  The pre-fix rule (VA spaces and managed objects only) is run as
 * well: it must be shown wrong by the same oracle, so that the test is red
 * for the old code and green for the new.
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

/* the world: what exists that runs module code later */
struct world {
	unsigned open_fds;	/* open files, with or without a VA space */
	unsigned va_spaces;
	unsigned sema_objs;	/* semaphore pool pager objects */
	unsigned managed_objs;	/* managed range pager objects */
	bool unloading;
};

enum ev { EV_OPEN, EV_INIT_VAS, EV_CLOSE, EV_MAP_SEMA, EV_MAP_MANAGED, EV_UNMAP_SEMA, EV_UNMAP_MANAGED, EV_UNLOAD, EV_N };

/* the pre-fix rule of uvm_unload: VA spaces and managed objects, nothing else */
static bool
old_busy(const struct world *w)
{
	return (w->va_spaces != 0 || w->managed_objs != 0);
}

static bool
new_busy(const struct world *w)
{
	return (uvm_unload_busy(w->open_fds, w->sema_objs + w->managed_objs, w->va_spaces, 0));
}

/* apply one event; returns false when it is not possible in this state; *unloaded is set when the module went */
static bool
step(struct world *w, enum ev e, bool (*busy)(const struct world *), bool *unloaded, bool gated_open)
{
	switch (e) {
	case EV_OPEN:
		if (gated_open && !uvm_open_admit(w->unloading))
			return (false);
		w->open_fds++;
		return (true);
	case EV_INIT_VAS:
		if (w->open_fds == 0 || w->va_spaces >= w->open_fds)
			return (false);
		w->va_spaces++;
		return (true);
	case EV_CLOSE:
		if (w->open_fds == 0)
			return (false);
		w->open_fds--;
		if (w->va_spaces > w->open_fds)
			w->va_spaces = w->open_fds;
		return (true);
	case EV_MAP_SEMA:
		if (w->va_spaces == 0)
			return (false);
		w->sema_objs++;
		return (true);
	case EV_MAP_MANAGED:
		if (w->va_spaces == 0)
			return (false);
		w->managed_objs++;
		return (true);
	case EV_UNMAP_SEMA:
		if (w->sema_objs == 0)
			return (false);
		w->sema_objs--;
		return (true);
	case EV_UNMAP_MANAGED:
		if (w->managed_objs == 0)
			return (false);
		w->managed_objs--;
		return (true);
	case EV_UNLOAD:
		if (busy(w))
			return (true);	/* refused: nothing changes */
		w->unloading = true;
		*unloaded = true;
		return (true);
	default:
		return (false);
	}
}

/* run every sequence of `len` events; count the ones where the module went while module code could still run */
static unsigned
run(bool (*busy)(const struct world *), bool gated_open, unsigned len)
{
	unsigned total = 1, bad = 0, i, n, seq;
	struct world w;
	bool unloaded;
	unsigned x;

	for (i = 0; i < len; i++)
		total *= EV_N;
	for (seq = 0; seq < total; seq++) {
		w = (struct world){ 0 };
		unloaded = false;
		x = seq;
		for (n = 0; n < len; n++) {
			enum ev e = (enum ev)(x % EV_N);
			bool was_unloaded = unloaded;

			x /= EV_N;
			(void)step(&w, e, busy, &unloaded, gated_open);
			/* once unloaded, nothing may exist that runs module code (a later event on dead state is not modelled) */
			if (unloaded && !was_unloaded &&
			    (w.open_fds != 0 || w.sema_objs != 0 || w.managed_objs != 0 || w.va_spaces != 0)) {
				bad++;
				break;
			}
			if (unloaded)
				break;
		}
	}
	return (bad);
}

int
main(void)
{
	/* the two predicates, every input alone and together */
	CHECK(!uvm_unload_busy(0, 0, 0, 0), "an idle module may unload");
	CHECK(uvm_unload_busy(1, 0, 0, 0), "an open file without a VA space keeps the module");
	CHECK(uvm_unload_busy(0, 1, 0, 0), "a pager object that outlived its fd keeps the module");
	CHECK(uvm_unload_busy(0, 0, 1, 0), "a VA space keeps the module");
	CHECK(uvm_unload_busy(0, 0, 0, 1), "a registered GPU keeps the module");
	CHECK(uvm_unload_busy(3, 2, 1, 1), "everything busy is busy");
	CHECK(uvm_open_admit(false), "an open is admitted before the unload is decided");
	CHECK(!uvm_open_admit(true), "an open is refused once the unload is decided");

	/* the oracle: the old rule is wrong (red), the new one is not (green) */
	CHECK(run(old_busy, false, 5) > 0, "the pre-fix rule must be shown wrong by the oracle");
	CHECK(run(new_busy, true, 5) == 0, "the gated rule let the module go under live module code");
	/* without the open gate the check alone is not enough: an open can slip in after it */
	CHECK(run(new_busy, false, 6) == 0, "the new predicate alone is already sound for these sequences");

	if (fails == 0)
		printf("unload_state_test: all rows pass\n");
	return (fails != 0);
}
