#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# giant_yield_log_test.sh - source-level checks for Track #2295:
#   - uvm_linux_ioctl runs the native ioctl off Giant (DROP_GIANT around
#     kern_ioctl), because the Linuxulator takes Giant around its handler;
#   - the page-table map and unmap loops yield (maybe_yield) and stop on a
#     pending signal (SIGPENDING / EINTR);
#   - the UUID-lookup and not-yet messages are rate limited, not plain
#     uvm_log.
# Run: sh tests/giant_yield_log_test.sh
set -eu
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
export PATH LC_ALL=C
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
S=$REPO/sys
rc=0
fail() { echo "FAIL $*"; rc=1; }

# Giant: DROP_GIANT precedes kern_ioctl and PICKUP_GIANT follows it
awk '/^uvm_linux_ioctl/ { f = 1 } f && /DROP_GIANT\(\)/ { d = NR } f && /kern_ioctl\(/ { k = NR }
    f && /PICKUP_GIANT\(\)/ { p = NR } END { exit !(d && k && p && d < k && k < p) }' "$S/uvm_main.c" ||
	fail "uvm_main.c: kern_ioctl is not wrapped in DROP_GIANT/PICKUP_GIANT"

# yield in the long loops; a pending-signal stop only in the map (init) loop
for fn in uvm_page_tree_write_ptes uvm_pt_range_vec_write uvm_pt_range_vec_clear uvm_pt_range_vec_clear_sub \
    uvm_pt_range_vec_init uvm_pt_range_vec_write_same; do
	awk -v fn="$fn" '$0 ~ "^" fn "\\(" { f = 1 } f && /uvm_pt_yield\(/ { y = 1 } f && /^}/ { exit(!y) }
	    END { if (!f) exit 2 }' "$S/uvm_page_tree.c" ||
		fail "uvm_page_tree.c: $fn does not call uvm_pt_yield"
done
awk '/^uvm_pt_range_vec_init/ { f = 1 } f && /uvm_pt_yield\(true\)/ { y = 1 } f && /^}/ { exit(!y) }' \
    "$S/uvm_page_tree.c" || fail "uvm_page_tree.c: the map loop does not stop on a pending signal"
grep -q 'maybe_yield()' "$S/uvm_page_tree.c" || fail "uvm_page_tree.c: no maybe_yield()"
grep -q 'sig_intr()' "$S/uvm_page_tree.c" || fail "uvm_page_tree.c: no pending-signal check"
grep -q 'NV_ERR_SIGNAL_PENDING' "$S/uvm_page_tree.c" || fail "uvm_page_tree.c: no signal status"

# logging
grep -n 'uvm_log("GPU lookup by UUID failed' "$S"/*.c >/dev/null 2>&1 &&
	fail "GPU lookup by UUID failed is not rate limited"
grep -q 'UVM_NOT_YET' "$S/uvm_ioctl.c" && ! grep -A1 'define.UVM_NOT_YET' "$S/uvm_ioctl.c" | grep -q 'uvm_log_slow' &&
	fail "UVM_NOT_YET is not slow-rate-limited"
[ $rc -eq 0 ] && echo "ok   giant_yield_log_test"
exit $rc
