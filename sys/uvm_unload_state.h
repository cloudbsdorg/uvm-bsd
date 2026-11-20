/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_unload_state.h - when the module may unload and when a new open may
 * start (Track #2272), as pure functions so that userland tests check every
 * row.
 *
 * What can still run module code after the last VA space is gone:
 *   - an open file (even one with no VA space yet) whose private data
 *     destructor has not run;
 *   - a device pager object (a semaphore pool or managed range mapping)
 *     whose destructor has not run: it outlives its fd, because the process
 *     map is torn down after its files are closed;
 *   - a thread that is returning from one of those destructors.
 * The first two are counted; the third is the last thing a destructor does
 * (the counter is dropped after every other access to module state) and the
 * unload waits for it behind a barrier.
 *
 * An open that races with the unload would create new state after the check,
 * so the check and the "unloading" mark are one step under uvm.lock and
 * uvm_open refuses once the mark is set.
 */

#ifndef UVM_UNLOAD_STATE_H
#define UVM_UNLOAD_STATE_H

#ifndef _KERNEL
#include <stdbool.h>
#endif

/* an open is admitted only before the unload has been decided */
static inline bool
uvm_open_admit(bool unloading)
{
	return (!unloading);
}

/*
 * uvm_unload_busy - true when the module must stay: a GPU is registered, a
 * VA space or an open file exists, or a pager object is still alive.
 */
static inline bool
uvm_unload_busy(unsigned open_fds, unsigned pager_objs, unsigned va_spaces, unsigned gpus)
{
	return (open_fds != 0 || pager_objs != 0 || va_spaces != 0 || gpus != 0);
}

#endif /* UVM_UNLOAD_STATE_H */
