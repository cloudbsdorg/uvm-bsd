#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# Copyright (c) 2026 SkunkWerks, GmbH
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
	# a test of the core is built together with the core source it checks
	extra=
	case "$name" in
	core_range_test|managed_state_test)	extra="$REPO/sys/core/uvm_core_range.c" ;;
	core_group_test)	extra="$REPO/sys/core/uvm_core_group.c" ;;
	core_device_test)	extra="$REPO/sys/core/uvm_core_device.c -lpthread" ;;
	esac
	# shellcheck disable=SC2086
	if ! cc -std=c11 -Wall -Wextra -Werror -I "$REPO/sys" -o "$T/$name" "$src" $extra; then
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
if cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I "$REPO/sys" -o "$T/uvm_owner_race" "$REPO/tests/ioctl/uvm_owner_race.c" -lpthread; then
	echo "ok   uvm_owner_race builds (run it with the modules loaded)"
else
	echo "FAIL uvm_owner_race does not compile"
	rc=1
fi
if cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I "$REPO/sys" -o "$T/uvm_owner_probe" "$REPO/tests/ioctl/uvm_owner_probe.c"; then
	echo "ok   uvm_owner_probe builds (run it as root with the modules loaded)"
else
	echo "FAIL uvm_owner_probe does not compile"
	rc=1
fi
if cc -std=c11 -Wall -Wextra -Werror -I "$REPO/sys" -o "$T/uvm_ioctl_test" "$REPO/tests/ioctl/uvm_ioctl_test.c"; then
	echo "ok   uvm_ioctl_test builds (run it with the modules loaded)"
else
	echo "FAIL uvm_ioctl_test does not compile"
	rc=1
fi
# the pure rules only protect the module if the kernel code calls them: a
# removed call leaves the userland tests green, so pin the call sites
for pat in 'sys/uvm_main.c:uvm_open_admit(uvm.unloading)' 'sys/uvm_main.c:uvm_unload_busy(' \
    'sys/uvm_main.c:uvm.unloading = true' 'sys/uvm_main.c:sysctl_ctx_free(&uvm_sysctl_ctx)' \
    'sys/uvm_mmap.c:uvm_pager_ctor_mem_refs()' 'sys/uvm_mmap.c:uvm_pager_mapper_drops_ref(' \
    'sys/uvm_main.c:MODULE_DEPEND(nvidia_uvm, nvidia, 2, 2, 2)' \
    'sys/uvm_gpu.c:uvm_fb_pma_absent_ok(status, &gpu->fb)'; do
	if grep -qF -- "${pat#*:}" "$REPO/${pat%%:*}"; then
		echo "ok   ${pat%%:*} uses ${pat#*:}"
	else
		echo "FAIL ${pat%%:*} does not use ${pat#*:}"
		rc=1
	fi
done
if sh "$REPO/tests/stress/uvm_stress.sh" -T >"$T/filter.out" 2>&1; then
	echo "ok   uvm_stress.sh -T (the kernel log filter)"
else
	cat "$T/filter.out"
	echo "FAIL uvm_stress.sh -T (the kernel log filter)"
	rc=1
fi
if sh "$REPO/tests/stress/uvm_stress_selftest.sh" >"$T/selftest.out" 2>&1; then
	echo "ok   uvm_stress_selftest.sh (fake-mode runs of the stress script)"
else
	cat "$T/selftest.out"
	echo "FAIL uvm_stress_selftest.sh"
	rc=1
fi
if sh -n "$REPO/tests/stress/uvm_stress.sh"; then
	echo "ok   uvm_stress.sh parses (run it with the modules loaded)"
else
	echo "FAIL uvm_stress.sh does not parse"
	rc=1
fi
for t in dma_tag_size_test giant_yield_log_test client_owner_pins_test client_owner_glue_test testboot_kit_test makefile_contract_test make_install_test; do
	if sh "$REPO/tests/$t.sh"; then :; else rc=1; fi
done
exit $rc
