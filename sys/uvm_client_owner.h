/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_client_owner.h - who may name an RM client handle (Track #2373), as
 * pure functions so that userland tests check every row.
 *
 * Five requests carry an rmCtrlFd and RM handles (01 section 5): register
 * GPU, register GPU VA space, register channel, map external allocation and
 * allocate device P2P memory.  RM resolves a handle with kernel privilege
 * and checks nothing about the caller, so the module must: a handle names
 * an RM client only for the process that holds the /dev/nvidiactl file that
 * client was allocated on.  Possession of that open file is the capability
 * (a dup of the fd, or the fd sent over a socket, is the same file).
 *
 * The rule, in the order the kernel applies it:
 *   1. the fd must resolve in the caller's table with read and ioctl rights;
 *   2. nvidia.ko must say the file is an nvidiactl file that owns hClient;
 *   3. any other answer, including "no such fd", is
 *      NV_ERR_INSUFFICIENT_PERMISSIONS (EPERM where a status becomes an
 *      errno), before any RM call.
 * Fail closed: the only path to NV_OK is a resolved fd and an owned handle.
 */

#ifndef UVM_CLIENT_OWNER_H
#define UVM_CLIENT_OWNER_H

#ifndef _KERNEL
#include <stdbool.h>
#endif

#include "nvidia_uvm_status.h"
#include "nvidia_uvm_ioctl.h"

/* the requests that carry an rmCtrlFd; tests/client_owner_pins_test.sh keeps this list and the header in step */
#define	UVM_OWNER_REQUESTS(X)	\
	X(UVM_OWNER_REGISTER_GPU)	\
	X(UVM_OWNER_REGISTER_GPU_VASPACE)	\
	X(UVM_OWNER_REGISTER_CHANNEL)	\
	X(UVM_OWNER_MAP_EXTERNAL_ALLOCATION)	\
	X(UVM_OWNER_ALLOC_DEVICE_P2P)

#define	UVM_OWNER_ENUM(n)	n,
enum uvm_owner_request { UVM_OWNER_REQUESTS(UVM_OWNER_ENUM) UVM_OWNER_NREQUESTS };
#undef UVM_OWNER_ENUM

/* does the request name an RM client at all */
static inline bool
uvm_owner_needed(enum uvm_owner_request req, NvHandle hClient, NvHandle hSmcPartRef)
{
	/*
	 * Register GPU reaches RM with a client only when it names one (or an
	 * SMC partition reference, which RM resolves through a client); with
	 * neither, no handle is resolved and there is nothing to own.  An SMC
	 * reference without its client is malformed: handle 0 is nobody's, so
	 * the check refuses it.  The
	 * other four always resolve the handle, handle 0 included, so they are
	 * always checked.
	 */
	if (req == UVM_OWNER_REGISTER_GPU)
		return (hClient != 0 || hSmcPartRef != 0);
	return (true);
}

/*
 * The verdict: fget_error is what resolving the fd returned (0 when it
 * resolved); owner_status is nvidia.ko's answer for the resolved file
 * (NV_OK only for an nvidiactl file that owns the handle).
 */
static inline NV_STATUS
uvm_owner_verdict(int fget_error, NV_STATUS owner_status)
{
	if (fget_error != 0)
		return (NV_ERR_INSUFFICIENT_PERMISSIONS);
	return (owner_status == NV_OK ? NV_OK : NV_ERR_INSUFFICIENT_PERMISSIONS);
}

#endif /* UVM_CLIENT_OWNER_H */
