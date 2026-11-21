#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# client_owner_glue_test.sh - Track #2373: behaviour tests of the glue in
# sys/uvm_ioctl.c that turns the ownership verdict into a refusal.  The
# comment-stripped text of uvm_client_hold, uvm_client_release, struct
# uvm_owner_hold and the five request handlers is extracted and compiled
# against fakes (tests/data/uvm_ioctl_glue_harness.c), then run.  Mutants of
# sys/uvm_ioctl.c (a refusal turned into success, the hold not asked, the
# release a no-op, the wrong handle or fd passed on, the file not dropped, the
# status of the hold ignored, the rights widened, the work run before the
# hold) must each turn the harness red.
#
# Run: sh tests/client_owner_glue_test.sh

set -eu
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
LC_ALL=C
export PATH LC_ALL
TESTSDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$TESTSDIR/.." && pwd)
WORK=${GB10_WORK:-$HOME/work/gb10}
mkdir -p "$WORK/tmp"
T=$(mktemp -d "$WORK/tmp/uvm-owner-glue.XXXXXXXX")
trap 'rm -rf "$T"' EXIT HUP INT QUIT TERM
. "$TESTSDIR/pins_lib.sh"
rc=0

# extract <sys/uvm_ioctl.c copy> <out>: the glue, comment-stripped
extract() {
	pin_strip "$1" > "$T/stripped.c"
	{
		awk '/^struct uvm_owner_hold \{/ { f = 1 } f { print } f && /^\};/ { f = 0 }' "$T/stripped.c"
		for fn in uvm_client_hold uvm_client_release uvm_h_register_gpu uvm_h_register_gpu_vaspace \
		    uvm_h_register_channel uvm_h_map_external_allocation uvm_h_alloc_device_p2p; do
			awk -v fn="$fn" '
			{ line[NR] = $0 }
			$0 ~ "^" fn "\\(" { s = NR - 1; f = 1 }
			f && /^}/ { for (i = s; i <= NR; i++) print line[i]; print ""; exit }' "$T/stripped.c"
		done
	} > "$2"
}

run_harness() {	# run_harness <uvm_ioctl.c copy> <out>: 0 ok, 1 harness failed, 2 did not build
	mkdir -p "$T/b"
	extract "$1" "$T/b/uvm_glue.inc"
	cc -std=gnu11 -Wall -Wextra -Wno-unused-function -Wno-unused-but-set-variable -Wno-unused-variable -Werror -I "$T/b" -I "$REPO/sys" \
	    -o "$T/b/h" "$REPO/tests/data/uvm_ioctl_glue_harness.c" >"$2" 2>&1 || return 2
	"$T/b/h" >"$2" 2>&1 || return 1
	return 0
}

if run_harness "$REPO/sys/uvm_ioctl.c" "$T/real.out"; then
	echo "ok   the real glue passes every scenario ($(grep -c . "$T/b/uvm_glue.inc") lines of glue)"
else
	cat "$T/real.out"; echo "FAIL the real glue fails the harness"; rc=1
fi

mutant() {	# mutant <name> <sed expression>
	sed -e "$2" "$REPO/sys/uvm_ioctl.c" > "$T/mut.c"
	if cmp -s "$REPO/sys/uvm_ioctl.c" "$T/mut.c"; then echo "FAIL mutant $1 changed nothing"; rc=1; return; fi
	run_harness "$T/mut.c" "$T/mut.out" && r=0 || r=$?
	case $r in
	1) echo "ok   mutant $1 is red" ;;
	2) echo "FAIL mutant $1 did not build: $(head -2 "$T/mut.out" | tr '\n' ' ')"; rc=1 ;;
	*) echo "FAIL mutant $1 stays green"; rc=1 ;;
	esac
}
mutant fget-error-returns-ok 's/return (uvm_owner_verdict(error, NV_OK));/return (NV_OK);/'
mutant hold-not-asked 's/owner = nvUvmInterfaceClientHold(fp, hClient);/owner = NV_OK;/'
mutant refusal-returned-as-ok 's/^	return (verdict);/	return (NV_OK);/'
mutant verdict-ignored 's/verdict = uvm_owner_verdict(0, owner);/verdict = NV_OK;/'
mutant limited-list-ignored 's/^	limited = caps.fc_ioctls != NULL;/	limited = 0;/'
mutant limited-branch-leaks-file 's/^\t\tfdrop(fp, td);$/\t\t(void)fp;/'
mutant release-is-a-noop 's/nvUvmInterfaceClientRelease(h->fp, h->hClient);/(void)h;/'
mutant release-wrong-handle 's/nvUvmInterfaceClientRelease(h->fp, h->hClient);/nvUvmInterfaceClientRelease(h->fp, h->hClient + 1);/'
mutant file-not-dropped 's/^	fdrop(fp, td);.*/	(void)td;/'
mutant rights-widened 's/cap_rights_init_one(&rights, CAP_IOCTL)/cap_rights_init_one(\&rights, CAP_IOCTL | CAP_READ)/'
mutant needed-check-off 's/if (!uvm_owner_needed(req, hClient, hSmcPartRef))/if (0 \&\& !uvm_owner_needed(req, hClient, hSmcPartRef))/'
mutant map-checks-handle-0 's/UVM_OWNER_MAP_EXTERNAL_ALLOCATION, p->rmCtrlFd, p->hClient/UVM_OWNER_MAP_EXTERNAL_ALLOCATION, p->rmCtrlFd, 0/'
mutant vaspace-checks-handle-0 's/UVM_OWNER_REGISTER_GPU_VASPACE, p->rmCtrlFd, p->hClient/UVM_OWNER_REGISTER_GPU_VASPACE, p->rmCtrlFd, 0/'
mutant channel-checks-handle-0 's/UVM_OWNER_REGISTER_CHANNEL, p->rmCtrlFd, p->hClient/UVM_OWNER_REGISTER_CHANNEL, p->rmCtrlFd, 0/'
mutant gpu-checks-handle-0 's/UVM_OWNER_REGISTER_GPU, p->rmCtrlFd, p->hClient/UVM_OWNER_REGISTER_GPU, p->rmCtrlFd, 0/'
mutant p2p-checks-handle-0 's/UVM_OWNER_ALLOC_DEVICE_P2P, p->rmCtrlFd, p->hClient/UVM_OWNER_ALLOC_DEVICE_P2P, p->rmCtrlFd, 0/'
mutant gpu-checks-fd-0 's/UVM_OWNER_REGISTER_GPU, p->rmCtrlFd,/UVM_OWNER_REGISTER_GPU, 0,/'
mutant channel-ignores-refusal '/status = uvm_client_hold(td, UVM_OWNER_REGISTER_CHANNEL/{n;N;d;}'
mutant vaspace-ignores-refusal '/status = uvm_client_hold(td, UVM_OWNER_REGISTER_GPU_VASPACE/{n;N;d;}'
mutant map-ignores-refusal '/status = uvm_client_hold(td, UVM_OWNER_MAP_EXTERNAL_ALLOCATION/{n;N;d;}'
mutant p2p-ignores-refusal '/status = uvm_client_hold(td, UVM_OWNER_ALLOC_DEVICE_P2P/{n;N;d;}'
mutant gpu-ignores-refusal '/status = uvm_client_hold(td, UVM_OWNER_REGISTER_GPU,/{n;N;d;}'
mutant channel-hold-commented 's|^	status = uvm_client_hold(td, UVM_OWNER_REGISTER_CHANNEL.*|	status = NV_OK; /* status = uvm_client_hold(td, UVM_OWNER_REGISTER_CHANNEL, p->rmCtrlFd, p->hClient, 0, \&hold); */|'
mutant channel-hold-disabled 's|^	status = uvm_client_hold(td, UVM_OWNER_REGISTER_CHANNEL.*|	status = 0 \&\& uvm_client_hold(td, UVM_OWNER_REGISTER_CHANNEL, p->rmCtrlFd, p->hClient, 0, \&hold);|'
mutant channel-release-dropped '/uvm_client_release(td, &hold);/{x;s/^/x/;/^xxx$/{x;d;};x;}'

if [ "$rc" = 0 ]; then echo "client_owner_glue_test.sh: all checks passed"; else echo "client_owner_glue_test.sh: FAILED"; fi
exit $rc
