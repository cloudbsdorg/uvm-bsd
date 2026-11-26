/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_bridge.h - the interface between nvidia-uvm.ko and an optional Linux
 * ioctl bridge module (nvidia_uvm_linux.ko, Track #2444).
 *
 * nvidia-uvm.ko is the native FreeBSD driver: it knows nothing of the
 * Linuxulator, imports no Linux symbol and does not depend on a Linux
 * module.  The bridge module is the other way round: it belongs to the
 * Linuxulator, owns the Linux request numbers and offers them to
 * third-party Linux CUDA binaries.  It needs the driver for one thing
 * only: to know whether a descriptor is one of the driver's two nodes, so
 * that it can answer ENOIOCTL for a foreign descriptor and let the
 * Linuxulator try the next handler.
 *
 * Versioning: the bridge module declares a dependency on the driver's
 * module version (MODULE_DEPEND), so a bridge built against another
 * revision of this interface refuses to load against a driver that does
 * not offer it.  A change to the interface below therefore bumps the
 * driver's MODULE_VERSION and the bridge's dependency range with it.
 */

#ifndef UVM_BRIDGE_H
#define UVM_BRIDGE_H

#include <sys/types.h>

struct thread;

/*
 * uvm_bridge_fd_is_ours - is the descriptor one of the driver's nodes?
 *
 * Returns 1 when the descriptor is /dev/nvidia-uvm or /dev/nvidia-uvm-tools,
 * 0 when it is another device, and a negative errno when the descriptor
 * cannot be examined at all (the caller hands that errno on unchanged).
 */
int uvm_bridge_fd_is_ours(struct thread *td, int fd);

#endif /* UVM_BRIDGE_H */
