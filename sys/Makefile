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
SRCS=	uvm_main.c uvm_ioctl.c uvm_va_space.c uvm_gpu.c uvm_channel.c uvm_backend_nvidia.c \
	uvm_page_tree.c uvm_faults.c uvm_mmap.c \
	uvm_core_range.c uvm_core_group.c uvm_core_device.c
SRCS+=	bus_if.h device_if.h vnode_if.h opt_global.h

CFLAGS+=	-I${.CURDIR} -I${.CURDIR}/core
# every warning is an error: a sign-compare or a missing prototype is a bug here
WERROR=	-Werror
CWARNFLAGS+=	-Wall -Wextra -Wno-unused-parameter -Wmissing-prototypes -Wstrict-prototypes

# the vendor-neutral core lives in its own directory; its sources are found
# there, so that its objects are built beside the module's other objects and
# the core's directory holds sources only
.PATH: ${.CURDIR}/core

# The base toolchain only.  A PATH with /usr/local/bin first picks up
# binutils' GNU ld and objcopy, and GNU ld links a .ko that the kernel's
# link_elf cannot resolve ("symbol nvUvmInterfaceChannelDestroy undefined");
# that cost three test boots on amp001 (2026-11-19/20).
LD=		/usr/bin/ld
OBJCOPY=	/usr/bin/objcopy

.include <bsd.kmod.mk>

# refuse a module that base lld did not link, whatever PATH or LD said
all: toolchain-check
toolchain-check: ${PROG}
	@if ! readelf -p .comment ${PROG} 2>/dev/null | grep -q 'Linker: LLD'; then \
		echo "${PROG}: not linked by the base lld (/usr/bin/ld); check PATH and LD" >&2; \
		rm -f ${PROG}; exit 1; \
	fi
