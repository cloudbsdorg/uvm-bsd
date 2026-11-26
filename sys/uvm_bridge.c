/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_bridge.c - the driver side of the optional Linux ioctl bridge
 * interface (Track #2444).
 *
 * nvidia-uvm.ko is the native FreeBSD driver.  It serves native callers
 * through its own request numbers (nvidia_uvm_ioctl.h) and knows nothing
 * of the Linuxulator.  An optional module (nvidia_uvm_linux.ko) offers
 * the Linux request numbers to third-party Linux CUDA binaries; it asks
 * the driver here whether a descriptor is one of the driver's nodes,
 * because only the driver knows its own device nodes.
 */

#include "uvm.h"
#include "uvm_bridge.h"

#include <sys/capsicum.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/vnode.h>

/*
 * uvm_bridge_fd_is_ours - is the descriptor one of the driver's nodes?
 *
 * The Linuxulator chooses a handler by request number alone, so a handler
 * must answer ENOIOCTL for a descriptor that is not its own: the ioctl of
 * another device (an NVIDIA device node of another driver, say) is not
 * ours to translate.
 *
 * The comparison is against the driver's own cdevs, which is exact: the
 * node names are not matched, so a renamed node cannot make the bridge
 * answer for a foreign device.  The module dependency of the bridge on
 * this driver keeps the nodes alive while the bridge is loaded (the kernel
 * refuses to unload a module that another loaded module depends on), so
 * the comparison cannot race a teardown.
 */
int
uvm_bridge_fd_is_ours(struct thread *td, int fd)
{
	struct file *fp;
	struct cdev *cdev;
	int ours = 0;
	int error;

	error = fget(td, fd, &cap_ioctl_rights, &fp);
	if (error != 0)
		return (-error);
	if (fp->f_type == DTYPE_VNODE && fp->f_vnode != NULL && fp->f_vnode->v_type == VCHR) {
		cdev = fp->f_vnode->v_rdev;
		ours = (cdev == uvm.cdev_main || cdev == uvm.cdev_tools);
	}
	fdrop(fp, td);
	return (ours);
}
