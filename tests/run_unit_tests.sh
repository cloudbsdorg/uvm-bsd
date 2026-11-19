#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# run_unit_tests.sh - build and run the userland unit tests of the pure
# parts (ABI layouts, RM structure layouts, method and PTE encoders) with
# -Werror, natively; then syntax-check them for amd64.  Exit 1 on any
# failure.  Run: sh tests/run_unit_tests.sh

set -eu
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
export PATH LC_ALL=C
TESTSDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$TESTSDIR/.." && pwd)
WORK=${GB10_WORK:-$HOME/work/gb10}
mkdir -p "$WORK/tmp"
T=$(mktemp -d "$WORK/tmp/uvm-unit.XXXXXXXX")
trap 'rm -rf "$T"' EXIT HUP INT QUIT TERM
rc=0
for src in "$REPO"/tests/unit/*_test.c; do
	name=$(basename "$src" .c)
	if ! cc -std=c11 -Wall -Wextra -Werror -I "$REPO/sys" -o "$T/$name" "$src"; then
		echo "FAIL $name: does not compile"
		rc=1
		continue
	fi
	if "$T/$name"; then
		echo "ok   $name"
	else
		echo "FAIL $name"
		rc=1
	fi
	if cc -target x86_64-unknown-freebsd16.0 -std=c11 -Wall -Wextra -Werror -I "$REPO/sys" -fsyntax-only "$src"; then
		echo "ok   $name (amd64 syntax)"
	else
		echo "FAIL $name (amd64 syntax)"
		rc=1
	fi
done
if cc -std=c11 -Wall -Wextra -Werror -I "$REPO/sys" -o "$T/uvm_ioctl_test" "$REPO/tests/ioctl/uvm_ioctl_test.c"; then
	echo "ok   uvm_ioctl_test builds (run it with the modules loaded)"
else
	echo "FAIL uvm_ioctl_test does not compile"
	rc=1
fi
exit $rc
