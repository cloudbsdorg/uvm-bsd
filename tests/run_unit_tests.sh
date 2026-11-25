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
    'sys/uvm_main.c:MODULE_DEPEND(nvidia_uvm, nvidia, 2, 2, 2)'; do
	if grep -qF -- "${pat#*:}" "$REPO/${pat%%:*}"; then
		echo "ok   ${pat%%:*} uses ${pat#*:}"
	else
		echo "FAIL ${pat%%:*} does not use ${pat#*:}"
		rc=1
	fi
done

# The frame buffer rules live in sys/uvm_fb_state.h, and the unit test above
# checks what they decide.  That only protects the module if the kernel code
# asks the rules instead of deciding on its own, so the two call sites are
# pinned: the registration must take the PMA path from the verdict, and the
# channel manager must take the GPFIFO path from the verdict and must read
# the tunable once.  A pin that cannot fail is decoration, so every pin is
# shown to be able to fail below: a mutant that ignores the helper result is
# built, and the pin must catch it.

# check_gpu_fb_wiring <file> - the registration takes the PMA path from the
# verdict of the helper, not from the raw status of the RM call.
check_gpu_fb_wiring() {
	_f=$1
	_rc=0
	for _pat in 'gpu->fb_state = uvm_fb_info_state(status, &gpu->fb);' \
	    'pma_verdict = uvm_fb_pma_judge(gpu->fb_state, status);' \
	    'if (pma_verdict.decision == UVM_FB_PMA_FAIL) {' \
	    'if (pma_verdict.decision == UVM_FB_PMA_ABSENT) {'; do
		grep -qF -- "$_pat" "$_f" || {
			echo "    missing: $_pat"
			_rc=1
		}
	done
	return $_rc
}

# check_channel_fb_wiring <file> - the channel manager takes the GPFIFO
# placement from the helper and reads the tunable once.
check_channel_fb_wiring() {
	_f=$1
	_rc=0
	for _pat in 'sysmem_requested = (uvm.sysmem_gpfifo != 0);' \
	    'place = uvm_fb_gpfifo_place(sysmem_requested, gpu->fb_state);' \
	    'mgr->gpfifo_sysmem = uvm_fb_gpfifo_is_sysmem(place);' \
	    'case UVM_FB_GPFIFO_SYSMEM_NO_VIDEO:'; do
		grep -qF -- "$_pat" "$_f" || {
			echo "    missing: $_pat"
			_rc=1
		}
	done
	# the tunable is read once per decision: a second read can make the
	# message and the placement disagree
	_n=$(grep -oE '[^_A-Za-z0-9]uvm\.sysmem_gpfifo' "$_f" | wc -l)
	if [ "$_n" -ne 1 ]; then
		echo "    the tunable is read $_n times, it must be read once"
		_rc=1
	fi
	return $_rc
}

# mutant_differs <original> <mutant> <name> - a mutant that is the same file
# as the original proves nothing.
mutant_differs() {
	if cmp -s "$1" "$2"; then
		echo "FAIL $3: the mutant is the same file as the original"
		return 1
	fi
	return 0
}

if check_gpu_fb_wiring "$REPO/sys/uvm_gpu.c"; then
	echo "ok   sys/uvm_gpu.c takes the PMA path from the frame buffer verdict"
else
	echo "FAIL sys/uvm_gpu.c does not take the PMA path from the frame buffer verdict"
	rc=1
fi
if check_channel_fb_wiring "$REPO/sys/uvm_channel.c"; then
	echo "ok   sys/uvm_channel.c takes the GPFIFO path from the frame buffer verdict"
else
	echo "FAIL sys/uvm_channel.c does not take the GPFIFO path from the frame buffer verdict"
	rc=1
fi

# the unit test must fail when the rules themselves are broken
mkdir -p "$T/mutant_hdr"
sed 's/return (UVM_FB_BROKEN);/return (UVM_FB_ABSENT);/' "$REPO/sys/uvm_fb_state.h" \
    >"$T/mutant_hdr/uvm_fb_state.h"
if ! mutant_differs "$REPO/sys/uvm_fb_state.h" "$T/mutant_hdr/uvm_fb_state.h" \
    "the broken-frame-buffer mutant"; then
	rc=1
elif ! cc -std=c11 -Wall -Wextra -Werror -I "$T/mutant_hdr" -I "$REPO/sys" \
    -o "$T/fb_mutant_absent" "$REPO/tests/unit/fb_state_test.c" 2>"$T/mutant_hdr/absent.err"; then
	echo "FAIL the broken-frame-buffer mutant does not compile"
	cat "$T/mutant_hdr/absent.err"
	rc=1
elif "$T/fb_mutant_absent" >"$T/mutant_hdr/absent.out" 2>&1; then
	echo "FAIL the fb_state test passes on a mutant that calls broken information absent"
	rc=1
else
	echo "ok   the fb_state test fails on a mutant that calls broken information absent"
fi
sed 's/return (UVM_FB_GPFIFO_SYSMEM_NO_VIDEO);/return (UVM_FB_GPFIFO_VIDEO);/' \
    "$REPO/sys/uvm_fb_state.h" >"$T/mutant_hdr/uvm_fb_state.h"
if ! mutant_differs "$REPO/sys/uvm_fb_state.h" "$T/mutant_hdr/uvm_fb_state.h" \
    "the GPFIFO placement mutant"; then
	rc=1
elif ! cc -std=c11 -Wall -Wextra -Werror -I "$T/mutant_hdr" -I "$REPO/sys" \
    -o "$T/fb_mutant_gpfifo" "$REPO/tests/unit/fb_state_test.c" 2>"$T/mutant_hdr/gpfifo.err"; then
	echo "FAIL the GPFIFO placement mutant does not compile"
	cat "$T/mutant_hdr/gpfifo.err"
	rc=1
elif "$T/fb_mutant_gpfifo" >"$T/mutant_hdr/gpfifo.out" 2>&1; then
	echo "FAIL the fb_state test passes on a mutant that puts the GPFIFO in video memory without video memory"
	rc=1
else
	echo "ok   the fb_state test fails on a mutant that puts the GPFIFO in video memory without video memory"
fi

# the pins must fail when the call sites ignore the helper result
mkdir -p "$T/mutant"
sed 's/if (pma_verdict.decision == UVM_FB_PMA_FAIL) {/if (0 \&\& pma_verdict.decision == UVM_FB_PMA_FAIL) {/' \
    "$REPO/sys/uvm_gpu.c" >"$T/mutant/uvm_gpu.c"
if ! mutant_differs "$REPO/sys/uvm_gpu.c" "$T/mutant/uvm_gpu.c" "the PMA call-site mutant"; then
	rc=1
elif check_gpu_fb_wiring "$T/mutant/uvm_gpu.c"; then
	echo "FAIL the PMA pin passes on a mutant that ignores the verdict"
	rc=1
else
	echo "ok   the PMA pin fails on a mutant that ignores the verdict"
fi
sed 's/mgr->gpfifo_sysmem = uvm_fb_gpfifo_is_sysmem(place);/mgr->gpfifo_sysmem = false;/' \
    "$REPO/sys/uvm_channel.c" >"$T/mutant/uvm_channel.c"
if ! mutant_differs "$REPO/sys/uvm_channel.c" "$T/mutant/uvm_channel.c" \
    "the GPFIFO call-site mutant"; then
	rc=1
elif check_channel_fb_wiring "$T/mutant/uvm_channel.c"; then
	echo "FAIL the GPFIFO pin passes on a mutant that ignores the verdict"
	rc=1
else
	echo "ok   the GPFIFO pin fails on a mutant that ignores the verdict"
fi
sed 's/sysmem_requested = (uvm.sysmem_gpfifo != 0);/sysmem_requested = (uvm.sysmem_gpfifo != 0); if (uvm.sysmem_gpfifo == 0) sysmem_requested = false;/' \
    "$REPO/sys/uvm_channel.c" >"$T/mutant/uvm_channel.c"
if ! mutant_differs "$REPO/sys/uvm_channel.c" "$T/mutant/uvm_channel.c" \
    "the tunable double-read mutant"; then
	rc=1
elif check_channel_fb_wiring "$T/mutant/uvm_channel.c"; then
	echo "FAIL the GPFIFO pin passes on a mutant that reads the tunable twice"
	rc=1
else
	echo "ok   the GPFIFO pin fails on a mutant that reads the tunable twice"
fi
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
