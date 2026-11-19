#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# amd64_cross_check.sh - compile every module source for amd64 with the
# kernel's own flags (cc -target x86_64-unknown-freebsd16.0, -Werror) on
# an arm64 host, against the same SYSDIR the arm64 build uses.  A build
# that only ever ran on one architecture hides sign, size and header
# differences; this is the amd64 half of "target amd64 and arm64".
#
# It is a compile check (no link): the kernel's amd64 objects are not on
# this host.  Run: sh tests/amd64_cross_check.sh [SYSDIR]

set -eu
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
export PATH LC_ALL=C

TESTSDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$TESTSDIR/.." && pwd)
SYSDIR=${1:-${SYSDIR:-/usr/src/sys}}
WORK=${GB10_WORK:-$HOME/work/gb10}
mkdir -p "$WORK/tmp"
T=$(mktemp -d "$WORK/tmp/uvm-amd64.XXXXXXXX")
trap 'rm -rf "$T"' EXIT HUP INT QUIT TERM

# the include layout bsd.kmod.mk gives a module: machine -> <arch>/include, x86 -> x86/include
ln -s "$SYSDIR/amd64/include" "$T/machine"
ln -s "$SYSDIR/x86/include" "$T/x86"
for h in opt_global.h opt_acpi.h; do : > "$T/$h"; done
# the interface headers a module build generates (bsd.kmod.mk does the same)
(cd "$T" && awk -f "$SYSDIR/tools/makeobjops.awk" "$SYSDIR/kern/bus_if.m" -h \
    && awk -f "$SYSDIR/tools/makeobjops.awk" "$SYSDIR/kern/device_if.m" -h \
    && awk -f "$SYSDIR/tools/vnode_if.awk" "$SYSDIR/kern/vnode_if.src" -h)

CFLAGS="-target x86_64-unknown-freebsd16.0 -fsyntax-only -O2 -pipe -fno-strict-aliasing -Werror -D_KERNEL \
-DKLD_MODULE -nostdinc -I$T -I$REPO/sys -I$SYSDIR -I$SYSDIR/contrib/ck/include -include $T/opt_global.h \
-fno-common -ffreestanding -fwrapv -fstack-protector -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse \
-msoft-float -fno-asynchronous-unwind-tables -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer \
-MD -MF /dev/null -std=gnu17 -Wall -Wextra -Wno-unused-parameter -Wmissing-prototypes -Wstrict-prototypes \
-Wpointer-arith -Wcast-qual -Wundef -Wno-pointer-sign -D__printf__=__freebsd_kprintf__ -Wmissing-include-dirs \
-Wno-unknown-pragmas -Wswitch -Wno-error=tautological-compare -Wno-error=empty-body \
-Wno-error=parentheses-equality -Wno-error=unused-function -Wno-error=pointer-sign \
-Wno-error=shift-negative-value -Wno-address-of-packed-member -Wno-format-zero-length"

rc=0
for src in "$REPO"/sys/uvm_*.c; do
	# shellcheck disable=SC2086
	if cc $CFLAGS "$src" 2> "$T/err"; then
		echo "ok   amd64: $(basename "$src")"
	else
		echo "FAIL amd64: $(basename "$src")"
		head -n 20 "$T/err"
		rc=1
	fi
done
exit $rc
