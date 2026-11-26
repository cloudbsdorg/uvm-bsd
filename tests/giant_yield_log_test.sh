#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# giant_yield_log_test.sh - source-level pins for Track #2295 on the kernel
# glue that a userland test cannot run (the behaviour of the loops, the
# signal unwind, the log limiter and the DMA size rule is tested by
# tests/unit/pt_loop_test.c from the same headers the module uses):
#   - the Linux ioctl bridge (linux/uvm_linux_bridge.c, the optional module
#     of Track #2444) runs kern_ioctl between DROP_GIANT and PICKUP_GIANT;
#   - the page-table loops call uvm_pt_yield, uvm_pt_yield yields and looks
#     for a signal, the map loop passes its `intr` through;
#   - the split of a partial unmap is the one caller that is not
#     interruptible (an interrupted split destroys the surviving parts);
#   - the three provokable messages use uvm_log_slow, which uses the limiter.
# Every pin is run against planted defects.  Run: sh tests/giant_yield_log_test.sh [sysdir]
set -eu
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
export PATH LC_ALL=C
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SYS=${1:-$REPO/sys}
. "$REPO/tests/pins_lib.sh"
# the Giant pin is about the bridge module, whose sources are not in sys/
PIN_EXTRA_DIRS=$REPO/linux
WORK=${GB10_WORK:-$HOME/work/gb10}
mkdir -p "$WORK/tmp"
PIN_TMP=$(mktemp -d "$WORK/tmp/uvm-pins.XXXXXXXX")
trap 'rm -rf "$PIN_TMP"' EXIT HUP INT QUIT TERM
PIN_N=0
rc=0
S=$SYS
fail() { echo "FAIL $*"; rc=1; return 1; }

pin_giant() {
	# the bridge module's source: in the scratch directory of a mutation
	# self-test it is copied next to the files of sys/, otherwise it is in
	# the module's own directory
	_f=$1/uvm_linux_bridge.c
	[ -f "$_f" ] || _f=$REPO/linux/uvm_linux_bridge.c
	_b=$(pin_fn "$_f" uvm_linux_ioctl)
	pin_dead "$_b" || return 1
	printf '%s\n' "$_b" | awk '
	    /^[ \t]*DROP_GIANT\(\);[ \t]*$/ { d = NR }
	    /^[ \t]*error = kern_ioctl\(/ { k = NR }
	    /^[ \t]*PICKUP_GIANT\(\);[ \t]*$/ { p = NR }
	    END { exit !(d && k && p && d < k && k < p) }'
}

LOOPS="uvm_page_tree_write_ptes uvm_pt_range_vec_write uvm_pt_range_vec_clear uvm_pt_range_vec_clear_sub uvm_pt_range_vec_write_same"
pin_yields() {
	for _f in $LOOPS; do
		_b=$(pin_fn "$1/uvm_page_tree.c" "$_f")
		pin_dead "$_b" && pin_has "$_b" '^[[:space:]]*(\(void\))?[[:space:]]*uvm_pt_yield\(false\);' || return 1
	done
	_b=$(pin_fn "$1/uvm_page_tree.c" uvm_pt_yield)
	pin_dead "$_b" || return 1
	pin_has "$_b" '^[[:space:]]*maybe_yield\(\);' || return 1
	pin_has "$_b" 'if \(intr && sig_intr\(\) != 0\)' || return 1
	pin_has "$_b" 'return \(NV_ERR_SIGNAL_PENDING\);' || return 1
	pin_has "$_b" 'return \(NV_OK\);'
}

pin_intr() {
	_b=$(pin_fn "$1/uvm_page_tree.c" uvm_pt_range_vec_init)
	pin_dead "$_b" || return 1
	pin_has "$_b" '^[[:space:]]*status = uvm_pt_vec_build\(&uvm_pt_vec_kops, &k, va, size, table_span, intr, &n\);' || return 1
	pin_has "$_b" 'bool intr' || return 1
	_k=$(pin_fn "$1/uvm_page_tree.c" uvm_pt_vec_kyield)
	pin_has "$_k" 'return \(uvm_pt_yield\(intr\)\);'
}

pin_split() {
	# split_part is the only caller that passes false; every other caller passes true
	_b=$(pin_fn "$1/uvm_va_space.c" uvm_ext_mapping_split_part)
	pin_dead "$_b" || return 1
	pin_has "$_b" 'uvm_pt_range_vec_init\(gvas->tree, p->start, end - start \+ 1, m->ptes.page_size, false, &p->ptes\)' || return 1
	_all=$(pin_strip "$1/uvm_va_space.c" | grep -c 'uvm_pt_range_vec_init(')
	_true=$(pin_strip "$1/uvm_va_space.c" | grep -c 'uvm_pt_range_vec_init(.*, true, &')
	[ "$_all" -eq 6 ] && [ "$_true" -eq 5 ]
}

pin_logs() {
	_h=$(pin_strip "$1/uvm.h")
	_m=$(printf '%s\n' "$_h" | awk '/define[ \t]+uvm_log_slow/ { f = 1 } f { print } f && !/\\$/ { exit }')
	pin_dead "$_m" || return 1
	pin_has "$_m" 'getmicrouptime\(&__slownow\)' || return 1
	pin_has "$_m" 'if \(uvm_ratelimit_ok\(&__slowtv, &__slowint, &__slownow\)\)' || return 1
	pin_has "$_m" '\{ UVM_LOG_SLOW_SECS, 0 \}' || return 1
	_g=$(pin_strip "$1/uvm_gpu.c")
	pin_has "$_g" 'uvm_log_slow\("GPU lookup by UUID failed' || return 1
	_i=$(pin_strip "$1/uvm_ioctl.c")
	pin_has "$_i" 'uvm_log_slow\("%s is not available yet' || return 1
	pin_has "$_i" 'uvm_log_slow\("%s: rmStatus' || return 1
	pin_dead "$_i"
}

for p in giant yields intr split logs; do
	"pin_$p" "$S" || fail "pin $p does not hold on $S"
done

# planted defects: each must be caught
pin_mutate "giant: DROP_GIANT commented out" pin_giant uvm_linux_bridge.c 's,^\([[:space:]]*\)DROP_GIANT();,\1/* DROP_GIANT(); */,'
pin_mutate "giant: PICKUP_GIANT commented out" pin_giant uvm_linux_bridge.c 's,^\([[:space:]]*\)PICKUP_GIANT();,\1// PICKUP_GIANT();,'
pin_mutate "giant: DROP_GIANT disabled" pin_giant uvm_linux_bridge.c 's,^\([[:space:]]*\)DROP_GIANT();,\1if (0) DROP_GIANT();,'
pin_mutate "giant: DROP_GIANT after the ioctl" pin_giant uvm_linux_bridge.c 's,^\([[:space:]]*\)DROP_GIANT();,\1(void)0;,;s,^\([[:space:]]*\)PICKUP_GIANT();,\1DROP_GIANT();,'
pin_mutate "yield: write_ptes yield commented out" pin_yields uvm_page_tree.c '/^uvm_page_tree_write_ptes/,/^}/s|(void)uvm_pt_yield(false);|/* & */|'
pin_mutate "yield: clear_sub yield removed" pin_yields uvm_page_tree.c '/^uvm_pt_range_vec_clear_sub/,/^}/s|(void)uvm_pt_yield(false);||'
pin_mutate "yield: write_same yield disabled" pin_yields uvm_page_tree.c '/^uvm_pt_range_vec_write_same/,/^}/s|(void)uvm_pt_yield(false);|(void)0;|'
pin_mutate "yield: maybe_yield commented out" pin_yields uvm_page_tree.c 's,^\([[:space:]]*\)maybe_yield();,\1/* maybe_yield(); */,'
pin_mutate "yield: signal check disabled" pin_yields uvm_page_tree.c 's,if (intr && sig_intr() != 0),if (0 \&\& intr \&\& sig_intr() != 0),'
pin_mutate "yield: signal never reported" pin_yields uvm_page_tree.c 's,return (NV_ERR_SIGNAL_PENDING);,return (NV_OK);,'
pin_mutate "intr: build result dropped" pin_intr uvm_page_tree.c 's,status = uvm_pt_vec_build(,(void)uvm_pt_vec_build(,'
pin_mutate "intr: intr forced off" pin_intr uvm_page_tree.c 's|table_span, intr, &n|table_span, false, \&n|'
pin_mutate "intr: glue ignores intr" pin_intr uvm_page_tree.c 's,uvm_pt_yield(intr),uvm_pt_yield(false),'
pin_mutate "split: split_part interruptible" pin_split uvm_va_space.c 's|m->ptes.page_size, false, &p->ptes|m->ptes.page_size, true, \&p->ptes|'
pin_mutate "split: another caller not interruptible" pin_split uvm_va_space.c 's|UVM_PAGE_SIZE_64K, true, &m->ptes|UVM_PAGE_SIZE_64K, false, \&m->ptes|'
pin_mutate "logs: limiter replaced by if (1)" pin_logs uvm.h 's|if (uvm_ratelimit_ok(&__slowtv, &__slowint, &__slownow))|if (1)|'
pin_mutate "logs: zero interval" pin_logs uvm.h 's|{ UVM_LOG_SLOW_SECS, 0 }|{ 0, 0 }|'
pin_mutate "logs: UUID message unlimited" pin_logs uvm_gpu.c 's,uvm_log_slow("GPU lookup,uvm_log("GPU lookup,'
pin_mutate "logs: not-yet message unlimited" pin_logs uvm_ioctl.c 's,uvm_log_slow("%s is not available,uvm_log("%s is not available,'
pin_mutate "logs: rmStatus unlimited" pin_logs uvm_ioctl.c 's,uvm_log_slow("%s: rmStatus,uvm_log("%s: rmStatus,'

[ $rc -eq 0 ] && echo "ok   giant_yield_log_test"
exit $rc
