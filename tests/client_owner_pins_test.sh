#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# client_owner_pins_test.sh - Track #2373.  The pure rule
# (tests/unit/client_owner_test.c) only protects the module if every request
# that carries an RM client handle calls it before RM sees the handle.  This
# pins the call sites:
#   (1) every struct of nvidia_uvm_ioctl.h with an rmCtrlFd member has an
#       entry in UVM_OWNER_REQUESTS (sys/uvm_client_owner.h), and the other
#       way round;
#   (2) the handler of each entry (uvm_h_<name>) calls uvm_client_hold with
#       that entry's enumerator before the call into the VA space code that
#       reaches RM, returns its refusal at once, and calls uvm_client_release
#       after the work;
#   (3) the functions that hand hClient to RM (DupAddressSpace, DupMemory,
#       RetainChannel, a UvmGpuClientInfo) are exactly the known ones; a new
#       one fails here until it is added to the known list (this catches the
#       known entry points, not every path to RM);
#   (4) plants: a removed check, a refusal that is not returned, a check moved
#       after the work, a removed release, a new rmCtrlFd request nobody
#       listed, a new hClient consumer and weakened rules each turn this test
#       red.
#
# Run: sh tests/client_owner_pins_test.sh

set -eu
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
LC_ALL=C
export PATH LC_ALL
TESTSDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$TESTSDIR/.." && pwd)
WORK=${GB10_WORK:-$HOME/work/gb10}
mkdir -p "$WORK/tmp"
T=$(mktemp -d "$WORK/tmp/client-owner-pins.XXXXXXXX")
trap 'rm -rf "$T"' EXIT HUP INT QUIT TERM

# check_tree <sys dir>: prints one line per problem; no output = clean
check_tree() {
	sysd=$1
	hdr=$sysd/nvidia_uvm_ioctl.h
	own=$sysd/uvm_client_owner.h
	ioc=$sysd/uvm_ioctl.c

	# (1) structs with rmCtrlFd, as UVM_OWNER_<name>
	awk '
	/^typedef struct/ { inside = 1; has = 0; next }
	inside && /rmCtrlFd/ { has = 1 }
	inside && /^} [A-Z0-9_]+;/ {
		n = $2; sub(/;$/, "", n)
		if (has && n ~ /_PARAMS$/) { sub(/_PARAMS$/, "", n); print "UVM_OWNER_" substr(n, 5) }
		inside = 0
	}' "$hdr" | sort > "$T/from_header"
	sed -n 's/^[[:space:]]*X(\(UVM_OWNER_[A-Z0-9_]*\)).*/\1/p' "$own" | sort > "$T/from_list"
	diff "$T/from_header" "$T/from_list" > "$T/d1" || sed 's/^/rmCtrlFd structs vs UVM_OWNER_REQUESTS: /' "$T/d1"

	# (2) the handler of each entry
	while read -r e; do
		fn=uvm_h_$(echo "${e#UVM_OWNER_}" | tr 'A-Z' 'a-z')
		awk -v fn="$fn" -v e="$e" '
		/^[a-z_0-9]+\(/ { cur = $0; sub(/\(.*/, "", cur) }
		{ line[NR] = $0 }
		cur == fn && /status = uvm_client_hold\(td, / && index($0, e ",") && !chk { chk = NR }
		cur == fn && /uvm_vas_[a-z_]+\(vas, params\)/ && !use { use = NR }
		cur == fn && /UVM_NOT_YET/ && !use { use = NR }
		cur == fn && /uvm_client_release\(td, &hold\)/ && !rel { rel = NR }
		END {
			if (!chk) print fn ": no status = uvm_client_hold(td, " e ", ...) call"
			else if (line[chk + 1] !~ /if \(status != NV_OK\)/ || line[chk + 2] !~ /return \(status\);/)
				print fn ": the refusal of uvm_client_hold is not returned (expected if (status != NV_OK) return (status); right after it)"
			else if (use && use < chk) print fn ": the check comes after the call that reaches RM"
			else if (!use) print fn ": no call into the VA space code found (pin out of date)"
			else if (!rel) print fn ": no uvm_client_release: the client stays held (a free of it blocks and the nvidiactl file leaks)"
			else if (rel < use) print fn ": the release comes before the work it guards"
		}' "$ioc"
	done < "$T/from_list"

	# (3) the consumers of hClient: functions that call the RM entry points with a user handle
	cat "$sysd"/*.c | awk '
	/^[a-z_0-9]+\(/ { cur = $0; sub(/\(.*/, "", cur) }
	/nvUvmInterfaceDupAddressSpace\(|nvUvmInterfaceDupMemory\(|nvUvmInterfaceRetainChannel\(|UvmGpuClientInfo client = \{ p->hClient/ { print cur }' | sort -u > "$T/consumers"
	printf '%s\n' uvm_ext_map_one uvm_register_channel_retain uvm_vas_register_gpu uvm_vas_register_gpu_va_space | sort > "$T/known"
	diff "$T/known" "$T/consumers" > "$T/d3" || sed 's/^/hClient consumers (< expected, > found; a new function hands hClient to RM: give its request a uvm_client_hold and add it to the known list): /' "$T/d3"
}

rc=0
out=$(check_tree "$REPO/sys")
if [ -z "$out" ]; then echo "ok   call sites of the client ownership check"; else echo "FAIL call sites:"; echo "$out"; rc=1; fi

plant() {	# plant <name> <file> <sed>
	mkdir -p "$T/p-$1"
	cp "$REPO"/sys/*.c "$REPO"/sys/*.h "$T/p-$1/"
	sed -e "$3" "$REPO/sys/$2" > "$T/p-$1/$2"
	if cmp -s "$REPO/sys/$2" "$T/p-$1/$2"; then echo "FAIL plant $1 did not change $2"; rc=1; return; fi
	if [ -z "$(check_tree "$T/p-$1")" ]; then echo "FAIL plant $1 stays green"; rc=1; else echo "ok   plant $1 is red"; fi
}
plant no-check-register-gpu uvm_ioctl.c 's/UVM_OWNER_REGISTER_GPU, p->rmCtrlFd/UVM_OWNER_NONE, p->rmCtrlFd/'
plant no-check-vaspace uvm_ioctl.c 's/UVM_OWNER_REGISTER_GPU_VASPACE, p->rmCtrlFd/UVM_OWNER_NONE, p->rmCtrlFd/'
plant no-check-channel uvm_ioctl.c 's/UVM_OWNER_REGISTER_CHANNEL, p->rmCtrlFd/UVM_OWNER_NONE, p->rmCtrlFd/'
plant no-check-map uvm_ioctl.c 's/UVM_OWNER_MAP_EXTERNAL_ALLOCATION, p->rmCtrlFd/UVM_OWNER_NONE, p->rmCtrlFd/'
plant no-check-p2p uvm_ioctl.c 's/UVM_OWNER_ALLOC_DEVICE_P2P, p->rmCtrlFd/UVM_OWNER_NONE, p->rmCtrlFd/'

plant ignore-status uvm_ioctl.c '/status = uvm_client_hold(td, UVM_OWNER_REGISTER_CHANNEL/{n;N;d;}'
plant no-release-first-handler uvm_ioctl.c '/uvm_client_release(td, &hold);/{x;s/^/x/;/^x$/{x;d;};x;}'
# the hold moved after the work in one handler: swap the two statements
mkdir -p "$T/p-moved-after"
cp "$REPO"/sys/*.c "$REPO"/sys/*.h "$T/p-moved-after/"
python3 -I - "$T/p-moved-after/uvm_ioctl.c" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
m = re.search(r"(\tstatus = uvm_client_hold\(td, UVM_OWNER_REGISTER_CHANNEL[^\n]*\n\tif \(status != NV_OK\)\n\t\treturn \(status\);\n)(\tstatus = uvm_vas_register_channel\(vas, params\);\n)", s)
s = s[:m.start()] + m.group(2) + m.group(1) + s[m.end():]
open(p, "w").write(s)
PY
if [ -z "$(check_tree "$T/p-moved-after")" ]; then echo "FAIL plant moved-after stays green"; rc=1; else echo "ok   plant moved-after is red"; fi
append_plant() {	# append_plant <name> <file> <text>
	mkdir -p "$T/p-$1"
	cp "$REPO"/sys/*.c "$REPO"/sys/*.h "$T/p-$1/"
	printf '%s\n' "$3" >> "$T/p-$1/$2"
	if [ -z "$(check_tree "$T/p-$1")" ]; then echo "FAIL plant $1 stays green"; rc=1; else echo "ok   plant $1 is red"; fi
}
append_plant unlisted-request nvidia_uvm_ioctl.h 'typedef struct {
	NvS32 rmCtrlFd;
	NV_STATUS rmStatus;
} UVM_NEW_THING_PARAMS;'
append_plant new-consumer uvm_va_space.c 'NV_STATUS
uvm_new_path(struct uvm_gpu *gpu, UVM_MAP_EXTERNAL_ALLOCATION_PARAMS *p)
{
	return (nvUvmInterfaceDupMemory(gpu->rm_device, p->hClient, p->hMemory, 0, 0));
}'

# the rule itself: the pure test must go red on a weakened rule
model_plant() {	# model_plant <name> <sed>
	mkdir -p "$T/m-$1"
	sed -e "$2" "$REPO/sys/uvm_client_owner.h" > "$T/m-$1/uvm_client_owner.h"
	if cmp -s "$REPO/sys/uvm_client_owner.h" "$T/m-$1/uvm_client_owner.h"; then echo "FAIL plant $1 did not change the header"; rc=1; return; fi
	if ! cc -std=c11 -Wall -Wextra -I "$T/m-$1" -I "$REPO/sys" -o "$T/m-$1/t" "$REPO/tests/unit/client_owner_test.c" 2>"$T/m-$1/err"; then
		echo "FAIL plant $1 does not compile: $(head -3 "$T/m-$1/err")"; rc=1; return
	fi
	if "$T/m-$1/t" >/dev/null 2>&1; then echo "FAIL plant $1 stays green"; rc=1; else echo "ok   plant $1 is red"; fi
}
model_plant verdict-ignores-owner 's/return (owner_status == NV_OK ? NV_OK : NV_ERR_INSUFFICIENT_PERMISSIONS);/return (NV_OK);/'
model_plant verdict-ignores-fget 's/if (fget_error != 0)/if (0)/'
model_plant never-needed 's/return (hClient != 0 || hSmcPartRef != 0);/return (false);/'
model_plant others-unchecked 's/^\treturn (true);/\treturn (false);/'
exit $rc
