#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# toolchain_test.sh - nvidia-uvm.ko must be linked by the base lld whatever
# PATH says.  binutils in /usr/local/bin (GNU ld) links a module the kernel's
# link_elf cannot resolve; on amp001 that failed three test boots.
#
# Builds copies of sys/ in a scratch directory with /usr/local/bin first in
# PATH:
#   (a) the real Makefile gives a module whose .comment says "Linker: LLD";
#   (b) control: the Makefile without the LD/OBJCOPY pin is refused by the
#       toolchain check (no .ko is left behind);
#   (c) control: without the pin and without the check the module really is
#       a GNU ld one, so (a) and (b) test something.
# Run: sh tests/toolchain_test.sh   (needs SYSDIR, default /usr/src/sys)

set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
SYSDIR=${SYSDIR:-/usr/src/sys}
: "${TMPDIR:=$HOME/work/gb10/tmp}"
PASSED=0 FAILED=0
pass() { PASSED=$((PASSED + 1)); echo "PASS: $*"; }
fail() { FAILED=$((FAILED + 1)); echo "FAIL: $*"; }

if [ ! -x /usr/local/bin/ld ]; then
	echo "toolchain_test: SKIP: no /usr/local/bin/ld (binutils) on this host"
	exit 0
fi
T=$(mktemp -d "$TMPDIR/uvm-toolchain-test.XXXXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
HOSTILE=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin

build() {	# build DIR -> 0 if make succeeded
	(cd "$1" && env PATH=$HOSTILE make SYSDIR="$SYSDIR" clean all) >"$1.log" 2>&1
}
linker() {	# linker KO -> LLD | GNU | none
	if [ ! -f "$1" ]; then echo none
	elif readelf -p .comment "$1" 2>/dev/null | grep -q 'Linker: LLD'; then echo LLD
	else echo GNU; fi
}

# (a) the real Makefile
cp -R "$REPO/sys" "$T/a"
if build "$T/a" && [ "$(linker "$T/a/nvidia-uvm.ko")" = LLD ]; then
	pass "(a) hostile PATH: the module is linked by the base lld"
else
	fail "(a) hostile PATH: linker $(linker "$T/a/nvidia-uvm.ko") (log $T/a.log)"
fi

# (b) no pin: the check must refuse it
cp -R "$REPO/sys" "$T/b"
sed -i '' -e '/^LD=/d' -e '/^OBJCOPY=/d' "$T/b/Makefile"
if ! build "$T/b" && [ ! -f "$T/b/nvidia-uvm.ko" ] &&
    grep -q 'not linked by the base lld' "$T/b.log"; then
	pass "(b) control: without the pin the toolchain check refuses the module"
else
	fail "(b) control: without the pin the build was not refused (linker $(linker "$T/b/nvidia-uvm.ko"))"
fi

# (c) no pin, no check: GNU ld really is what a hostile PATH gives
cp -R "$REPO/sys" "$T/c"
sed -i '' -e '/^LD=/d' -e '/^OBJCOPY=/d' -e '/^all: toolchain-check/d' "$T/c/Makefile"
build "$T/c"
if [ "$(linker "$T/c/nvidia-uvm.ko")" = GNU ]; then
	pass "(c) control: without pin and check a hostile PATH links with GNU ld"
else
	fail "(c) control: expected a GNU ld module, got $(linker "$T/c/nvidia-uvm.ko")"
fi

echo "toolchain_test: $PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
