/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_linux_bridge.c - nvidia_uvm_linux.ko, the optional Linux ioctl
 * bridge of nvidia-uvm.ko (Track #2444).
 *
 * nvidia-uvm.ko is the native FreeBSD driver: it answers the request
 * numbers of nvidia_uvm_ioctl.h to native callers, and it depends on no
 * Linux module.  This module is the other way round: it belongs to the
 * Linuxulator, it owns the Linux request numbers
 * (nvidia_uvm_linux_ioctl.h), and it registers a Linuxulator ioctl
 * handler that offers those numbers to third-party Linux CUDA binaries.
 * Loading it is the operator's choice; a native FreeBSD caller (our
 * libcuda, Track #2428) needs the driver alone.
 *
 * The handler translates the Linux request number into the driver's own
 * and re-enters the kernel's own ioctl path with it, so that the
 * per-file state (devfs cdevpriv) is found the same way on both paths.
 *
 * Unload order: this module depends on nvidia-uvm.ko, so kldunload of the
 * driver is refused (EBUSY) while this one is loaded.  Unloading this
 * module waits out any Linux ioctl in the handler: the Linuxulator holds
 * its own lock across a handler call, and unregistering the handler takes
 * it exclusively.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/ioccom.h>
#include <sys/syscallsubr.h>
#include <sys/sysproto.h>

#include <machine/../linux/linux.h>
#include <machine/../linux/linux_proto.h>
#include <compat/linux/linux_ioctl.h>

#include "nvidia_uvm_ioctl.h"
#include "nvidia_uvm_linux_ioctl.h"
#include "uvm_bridge.h"

static int
uvm_linux_ioctl(struct thread *td, struct linux_ioctl_args *args)
{
	uintptr_t arg = args->arg;
	uint32_t request;
	int error;

	/*
	 * Is this descriptor one of the driver's nodes?  Anything else is not
	 * ours: ENOIOCTL lets the Linuxulator try the next handler.
	 */
	error = uvm_bridge_fd_is_ours(td, args->fd);
	if (error < 0)
		return (-error);
	if (error == 0)
		return (ENOIOCTL);

	/*
	 * A Linux request number the driver has no request for is sent as
	 * UVM_REQ_NONE, and the driver answers it as it answers any unknown
	 * request: ENOSYS on the main node, EINVAL on the tools node.
	 */
	request = uvm_linux_request_id((uint32_t)args->cmd);
	if (request == 0)
		request = UVM_REQ_NONE;

	/*
	 * The Linuxulator takes Giant around every registered handler.  Nothing
	 * below needs it (the driver uses sx, mutex and sleep locks of its
	 * own; the task queues are not Giant-bound), and an ioctl can run for
	 * a long time, so run it off Giant: USB enumeration and the other Giant
	 * users must not wait behind a GPU mapping (#2295).
	 */
	DROP_GIANT();
	error = kern_ioctl(td, args->fd, UVM_IOCTL_NATIVE(request), (caddr_t)&arg);
	PICKUP_GIANT();
	return (error);
}

static struct linux_ioctl_handler uvm_linux_handler = {
	.func = uvm_linux_ioctl,
	.low = LINUX_UVM_IOCTL_MIN,
	.high = LINUX_UVM_IOCTL_MAX,
};

static int
uvm_linux_load(void)
{
	return (linux_ioctl_register_handler(&uvm_linux_handler));
}

static int
uvm_linux_unload(void)
{
	linux_ioctl_unregister_handler(&uvm_linux_handler);
	return (0);
}

static int
uvm_linux_modevent(module_t mod, int what, void *arg)
{
	(void)mod;
	(void)arg;

	switch (what) {
	case MOD_LOAD:
		return (uvm_linux_load());
	case MOD_UNLOAD:
		return (uvm_linux_unload());
	case MOD_QUIESCE:
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t uvm_linux_mod = { "nvidia_uvm_linux", uvm_linux_modevent, NULL };
DECLARE_MODULE(nvidia_uvm_linux, uvm_linux_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(nvidia_uvm_linux, 1);
/* the driver this bridge serves (nvidia-uvm.ko) */
MODULE_DEPEND(nvidia_uvm_linux, nvidia_uvm, 1, 1, 1);
/*
 * The 64-bit Linuxulator: it exports the MODULE_VERSION interface
 * linux64elf on arm64 and linux64 on amd64 (linux64elf is only a module
 * name there, which a dependency does not resolve against), and both need
 * linux_common.  A 32-bit Linux binary on amd64 is not served: that needs
 * the linux interface.
 */
#if defined(__aarch64__)
MODULE_DEPEND(nvidia_uvm_linux, linux64elf, 1, 1, 1);
#elif defined(__amd64__)
MODULE_DEPEND(nvidia_uvm_linux, linux64, 1, 1, 1);
#else
#error "the UVM Linux ioctl bridge supports arm64 and amd64 only"
#endif
MODULE_DEPEND(nvidia_uvm_linux, linux_common, 1, 1, 1);
