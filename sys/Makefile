# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# nvidia-uvm.ko: the clean-room UVM driver for FreeBSD (uvm-spec r1).
# Build against the kernel source the running kernel was built from:
#   make SYSDIR=/path/to/sys
# The module links against nvidia.ko's exported nvUvmInterface* symbols
# (nvidia-gb10-freebsd, branch agent/uvm-phase0 or later) and the
# Linuxulator (linux64elf on arm64, linux on amd64) plus linux_common.

KMOD=	nvidia-uvm
SRCS=	uvm_main.c uvm_ioctl.c uvm_va_space.c uvm_gpu.c uvm_channel.c \
	uvm_page_tree.c uvm_faults.c uvm_mmap.c
SRCS+=	bus_if.h device_if.h vnode_if.h opt_global.h

CFLAGS+=	-I${.CURDIR}
# every warning is an error: a sign-compare or a missing prototype is a bug here
WERROR=	-Werror
CWARNFLAGS+=	-Wall -Wextra -Wno-unused-parameter -Wmissing-prototypes -Wstrict-prototypes

.include <bsd.kmod.mk>
