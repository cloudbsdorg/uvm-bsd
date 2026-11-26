# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# nvidia_uvm_linux.ko: the optional Linux ioctl bridge of nvidia-uvm.ko
# (Track #2444).  It belongs to the Linuxulator: it depends on nvidia-uvm.ko
# and on the Linuxulator, and it offers the Linux UVM request numbers to
# third-party Linux CUDA binaries.  nvidia-uvm.ko itself is the native
# FreeBSD driver and needs no Linux module.
#
# Build against the kernel source the running kernel was built from:
#   make SYSDIR=/path/to/sys
# The user's ABI header (the driver's request numbers) lives in ../sys.

KMOD=	nvidia_uvm_linux
SRCS=	uvm_linux_bridge.c

CFLAGS+=	-I${.CURDIR}/../sys

# every warning is an error: a sign-compare or a missing prototype is a bug here
WERROR=	-Werror
CWARNFLAGS+=	-Wall -Wextra -Wno-unused-parameter -Wmissing-prototypes -Wstrict-prototypes

# The base toolchain only.  A PATH with /usr/local/bin first picks up
# binutils' GNU ld and objcopy, and GNU ld links a .ko that the kernel's
# link_elf cannot resolve; that cost three test boots on amp001 (2026-11-19/20).
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
