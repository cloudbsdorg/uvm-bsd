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
. "$TESTSDIR/pins_lib.sh"

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

	# (2) the handler of each entry, on comment-stripped text
	pin_strip "$ioc" > "$T/ioc.stripped"
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
		}' "$T/ioc.stripped"
		pin_dead "$(pin_fn "$ioc" "$fn")" || echo "$fn: a disabled-code form (if (0, 0 &&, || 1) in the handler"
	done < "$T/from_list"
	pin_strip "$sysd/uvm_main.c" | grep -qF 'MODULE_DEPEND(nvidia_uvm, nvidia, 2, 2, 2);' || echo "uvm_main.c: no live MODULE_DEPEND(nvidia_uvm, nvidia, 2, 2, 2) (a comment does not count)"
	pin_dead "$(pin_fn "$ioc" uvm_client_hold)" || echo "uvm_client_hold: a disabled-code form"
	pin_dead "$(pin_fn "$ioc" uvm_client_release)" || echo "uvm_client_release: a disabled-code form"

	# (3) the consumers of hClient: functions that call the RM entry points with a user handle
	for f in "$sysd"/*.c; do pin_strip "$f"; done | awk '
	/^[a-z_0-9]+\(/ { cur = $0; sub(/\(.*/, "", cur) }
	/nvUvmInterfaceDupAddressSpace\(|nvUvmInterfaceDupMemory\(|nvUvmInterfaceRetainChannel\(|[.>]hClient = p->hClient|UvmGpuClientInfo [a-z_]+ = \{ p->hClient/ { print cur }' | sort -u > "$T/consumers"
	printf '%s\n' uvm_ext_map_one uvm_register_channel_retain uvm_vas_register_gpu uvm_vas_register_gpu_va_space uvm_vas_register_channel | sort > "$T/known"
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

plant commented-hold uvm_ioctl.c 's|^	status = uvm_client_hold(td, UVM_OWNER_REGISTER_CHANNEL.*|	status = NV_OK; /* status = uvm_client_hold(td, UVM_OWNER_REGISTER_CHANNEL, p->rmCtrlFd, p->hClient, 0, \&hold); */|'
plant disabled-hold uvm_ioctl.c 's|^	if (status != NV_OK)$|	if (0 \&\& status != NV_OK)|'
plant module-depend-commented uvm_main.c 's|^MODULE_DEPEND(nvidia_uvm, nvidia, 2, 2, 2);|/* MODULE_DEPEND(nvidia_uvm, nvidia, 2, 2, 2); */|'
plant ignore-status uvm_ioctl.c '/status = uvm_client_hold(td, UVM_OWNER_REGISTER_CHANNEL/{n;N;d;}'
plant no-release-first-handler uvm_ioctl.c '/uvm_client_release(td, &hold);/{x;s/^/x/;/^x$/{x;d;};x;}'
# the hold moved after the work in one handler: swap the two statements
mkdir -p "$T/p-moved-after"
cp "$REPO"/sys/*.c "$REPO"/sys/*.h "$T/p-moved-after/"
if python3 -I - "$T/p-moved-after/uvm_ioctl.c" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
m = re.search(r"(\tstatus = uvm_client_hold\(td, UVM_OWNER_REGISTER_CHANNEL[^\n]*\n\tif \(status != NV_OK\)\n\t\treturn \(status\);\n)(\tstatus = uvm_vas_register_channel\(vas, params\);\n)", s)
if m is None:
    sys.exit(3)
s = s[:m.start()] + m.group(2) + m.group(1) + s[m.end():]
open(p, "w").write(s)
PY
then
	if [ -z "$(check_tree "$T/p-moved-after")" ]; then echo "FAIL plant moved-after stays green"; rc=1; else echo "ok   plant moved-after is red"; fi
else
	echo "FAIL plant moved-after: the handler no longer has the shape the plant rewrites (update the plant)"; rc=1
fi
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
append_plant new-consumer-assign uvm_va_space.c 'NV_STATUS
uvm_new_assign(struct uvm_gpu *gpu, UVM_MAP_EXTERNAL_ALLOCATION_PARAMS *p)
{
	UvmGpuClientInfo client;

	client.hClient = p->hClient;
	return (nvUvmInterfaceGetGpuInfo(gpu, &client));
}'
append_plant new-consumer uvm_va_space.c 'NV_STATUS
uvm_new_path(struct uvm_gpu *gpu, UVM_MAP_EXTERNAL_ALLOCATION_PARAMS *p)
{
	return (nvUvmInterfaceDupMemory(gpu->rm_device, p->hClient, p->hMemory, 0, 0));
}'

# the probe keeps its control rows: without them a module that refuses everyone would pass
probe_rows() {	# probe_rows <probe source>: prints each missing row
	for row in "own fd, own handle (owner)" "a dup of own fd, own handle (the same file)" "a dup of own fd, the other's handle" \
	    "a dup of own fd with a limited ioctl list, own handle" "a dup of own fd with an EMPTY ioctl list, own handle" \
	    "the other's nvidiactl file passed to us"; do
		grep -qF -- "$row" "$1" || echo "uvm_owner_probe.c lost the row: $row"
	done
}
if [ -z "$(probe_rows "$REPO/tests/ioctl/uvm_owner_probe.c")" ]; then echo "ok   the probe keeps its control rows"; else echo "FAIL probe rows:"; probe_rows "$REPO/tests/ioctl/uvm_owner_probe.c"; rc=1; fi
grep -v 'own fd, own handle (owner)' "$REPO/tests/ioctl/uvm_owner_probe.c" > "$T/probe.mut"
if [ -z "$(probe_rows "$T/probe.mut")" ]; then echo "FAIL plant probe-owner-row-dropped stays green"; rc=1; else echo "ok   plant probe-owner-row-dropped is red"; fi

grep -v 'a dup of own fd with a limited ioctl list, own handle' "$REPO/tests/ioctl/uvm_owner_probe.c" > "$T/probe.mut2"
if cmp -s "$REPO/tests/ioctl/uvm_owner_probe.c" "$T/probe.mut2"; then echo "FAIL plant probe-limited-row-dropped changed nothing"; rc=1
elif [ -z "$(probe_rows "$T/probe.mut2")" ]; then echo "FAIL plant probe-limited-row-dropped stays green"; rc=1; else echo "ok   plant probe-limited-row-dropped is red"; fi

# the race loops prove nothing when they did nothing: the vacuity rule is in one place and sets "bad"
RACE=$REPO/tests/ioctl/uvm_owner_race.c
race_pins() {	# race_pins <source>: prints each problem
	pin_strip "$1" > "$T/race.stripped"
	# the vacuity rule is run, not read: the function is cut out of the source and fed rows
	awk '/^vacuous_loop\(/ { p = 1; print "static int" } p { print } p && /^}/ { exit }' "$T/race.stripped" > "$T/vac.fn"
	{ cat "$T/vac.fn"; cat <<'CEOF'
#include <stdio.h>
#define	ROW(l, a, b, o, want) do { if (vacuous_loop(l, a, b, o) != want) { printf("vacuous_loop(%d, %d, %d, %d) is not %d\n", l, a, b, o, want); bad = 1; } } while (0)
int main(void)
{
	int bad = 0;

	ROW(4, 1, 1, 0, 0); ROW(4, 3, 1, 2, 0); ROW(4, 5, 1, 4, 0); ROW(4, 5, 1, 5, 1); ROW(4, 5, 1, 6, 1);
	ROW(4, 0, 1, 0, 1); ROW(1, 5, 0, 0, 1); ROW(2, 0, 5, 0, 1); ROW(3, 5, 5, 100, 0); ROW(5, 5, 0, 0, 1);
	return (bad);
}
CEOF
	} > "$T/vac.c"
	if ! cc -std=c11 -Wall -Wextra -o "$T/vac" "$T/vac.c" 2>"$T/vac.err"; then echo "the vacuity rule does not compile as cut out: $(head -1 "$T/vac.err")"; elif ! "$T/vac" > "$T/vac.out" 2>&1; then echo "the vacuity rule: $(head -2 "$T/vac.out" | tr '\n' ' ')"; fi
	grep -qF 'int vac = vacuous_loop(loop, a, b, (long)atomic_load(&open_failed));' "$T/race.stripped" || echo "report() does not ask the vacuity rule"
	awk '/^\tif \(vac\) \{/ { getline nxt; if (nxt ~ /atomic_store\(&bad, 1\);/) ok = 1 } END { exit !ok }' "$T/race.stripped" || echo "a vacuous loop does not set bad"
	grep -qF 'case 4: return (loop4());' "$T/race.stripped" && grep -qF 'case 5: return (loop5());' "$T/race.stripped" || echo "loops 4 and 5 are not both run"
	grep -qF 'RETRIES' "$T/race.stripped" && grep -qF 'atomic_fetch_add(&sh->stuck, 1);' "$T/race.stripped" || echo "loop 5 does not count a client that stays refused"
	grep -qF 'alarm((unsigned)(seconds * wanted_loops() + 40));' "$T/race.stripped" || echo "the watchdog does not scale with the loops that run"
	grep -qF 'for (c = 1; c <= NLOOPS; c++) {' "$T/race.stripped" || echo "main does not run NLOOPS loops"
	grep -qF 'kill(*p1, SIGKILL);' "$T/race.stripped" || echo "spawn_two leaves the first child running when the second fork fails"
	grep -qF 'atomic_store(&iters_b, atomic_load(&sh->s_hits));' "$T/race.stripped" || echo "loop 5 does not report the stranger's contacts (a loop with none is vacuous)"
	grep -qF 'r = rm_free_client(c.ctl, h + k);' "$T/race.stripped" && grep -qF 'if (r == FREE_REFUSED)' "$T/race.stripped" || echo "the stranger does not count the frees that met a client"
	grep -qF 'become(UID_STRANGER)' "$T/race.stripped" && grep -qF 'become(UID_VICTIM)' "$T/race.stripped" || echo "loop 5 does not use its dedicated uids"
	grep -qF 'atomic_store(&last_bad, failed' "$T/race.stripped" && grep -qF 'atomic_store(&bad, 0);' "$T/race.stripped" || echo "a loop verdict leaks into the next loop (bad is not reset by report)"
	grep -qF 'atomic_fetch_add(&sh->s_freed, 1);' "$T/race.stripped" && grep -qF 'atomic_load(&sh->s_freed) != 0' "$T/race.stripped" || echo "loop 5 does not fail when the stranger frees a client it does not own"
	grep -qF 'atomic_fetch_add(&sh->v_free_refused, 1);' "$T/race.stripped" || echo "loop 5 does not fail when the victim is refused the free of its own client"
	grep -qF 'int failed = atomic_load(&bad);' "$T/race.stripped" && grep -qF '\"failed\":%s' "$T/race.stripped" || echo "report() does not emit failed before the vacuity rule hides it"
	grep -qF 'RE-RUN: loop %d is vacuous' "$T/race.stripped" || echo "a vacuous loop is reported as FAIL on the diagnostics"
	grep -qF 'atomic_store(&last_bad, failed ? 1 : (vac ? 4 : 0));' "$T/race.stripped" || echo "a vacuous-only loop does not have its own status (4), apart from a failed one (1)"
	grep -qF 'FAIL: loop 5: the victim or the stranger did not exit 0' "$T/race.stripped" && grep -qF 'FAIL: loop 4: the allocator process did not exit 0' "$T/race.stripped" || echo "a child that died leaves no reason on the diagnostics"
	grep -qF 'fprintf(diag(), "FAIL: the stranger freed' "$T/race.stripped" || echo "diagnostics of loop 5 do not go through diag() (they would sit in the JSON lines)"
}
if [ -z "$(race_pins "$RACE")" ]; then echo "ok   race loops: the vacuity rule (run on rows) and loop 5"; else echo "FAIL race loops:"; race_pins "$RACE"; rc=1; fi
race_plant() {	# race_plant <name> <sed>
	sed -e "$2" "$RACE" > "$T/race.mut"
	if cmp -s "$RACE" "$T/race.mut"; then echo "FAIL plant $1 changed nothing"; rc=1; return; fi
	if [ -z "$(race_pins "$T/race.mut")" ]; then echo "FAIL plant $1 stays green"; rc=1; else echo "ok   plant $1 is red"; fi
}
race_plant open-failures-ignored 's/ || (loop == 4 \&\& 2 \* open_fail >= a + open_fail)//'
race_plant open-failures-off-by-one 's/2 \* open_fail >= a + open_fail/2 * open_fail > a + open_fail/'
race_plant zero-b-passes 's/a == 0 || b == 0 ||/a == 0 ||/'
race_plant vacuous-not-bad '/^	if (vac) {/{n;s/.*/\t\t(void)vac;/;}'
race_plant stuck-not-counted 's/atomic_fetch_add(&sh->stuck, 1);/(void)sh;/'
race_plant loop5-not-run 's/case 5: return (loop5());/case 5: return (loop4());/'
race_plant watchdog-four-loops 's/seconds \* wanted_loops() + 40/seconds * 4 + 40/'
race_plant orphan-left-running 's/kill(\*p1, SIGKILL);/(void)p1;/'
race_plant contacts-not-reported 's/atomic_load(&sh->s_hits)/atomic_load(\&sh->s_frees)/'
race_plant shared-uids 's/become(UID_STRANGER)/become(UID_VICTIM)/'
race_plant vacuous-as-failed-status 's/failed ? 1 : (vac ? 4 : 0)/(failed || vac) ? 1 : 0/'
race_plant dead-child-silent 's/FAIL: loop 5: the victim or the stranger did not exit 0/loop 5/'
race_plant bad-not-reset 's/^	atomic_store(&bad, 0);$/	(void)0;/'
race_plant stranger-free-uncounted 's/atomic_fetch_add(&sh->s_freed, 1);/(void)sh;/'
race_plant victim-free-unchecked 's/atomic_fetch_add(&sh->v_free_refused, 1);/(void)sh;/'
race_plant failed-hidden 's/int failed = atomic_load(&bad);/int failed = 0;/'
race_plant vacuous-as-fail 's/RE-RUN: loop %d is vacuous/FAIL: loop %d is vacuous/'

# the boot script reads the probe and the loops the agreed way
BOOT=$REPO/tests/ioctl/run_owner_boot.sh
boot_pins() {	# boot_pins <script> <probe source>
	grep -qF '"refusal":true' "$1" || echo "the boot script does not read the probe's refusal field"
	grep -qF 'say "RE-RUN"' "$1" || echo "the boot script cannot end RE-RUN"
	grep -qF 'turns RM client validation off' "$1" || echo "the boot script does not refuse a host with RM client validation switched off"
	{ grep -qF 'RmValidateClientData' "$1" && ! grep -v '^[[:space:]]*#' "$1" | grep -qF 'RmClientDataValidation'; } || echo "the boot script does not read the RM registry key RmValidateClientData (and only that name)"
	grep -qF 'another process holds /dev/nvidiactl' "$1" && grep -qF 'fstat.race' "$1" || echo "the boot script does not look for another holder of /dev/nvidiactl just before the race loops"
	grep -qF '\"refusal\"' "$2" || echo "the probe does not print the refusal field"
	# the row reading is run on fixtures: failed beats vacuous, vacuous alone is RE-RUN, a clean row says nothing
	sed -n '/^# BEGIN read_race_rows/,/^# END read_race_rows/p' "$1" > "$T/rrr.sh"
	[ -s "$T/rrr.sh" ] || { echo "the boot script has no read_race_rows function"; return; }
	for fx in 'failed-and-vacuous|{"schema":"uvm-owner-race/1","loop":5,"what":"w","iterations_a":3,"iterations_b":0,"open_failed":0,"failed":true,"vacuous":true,"ok":false}|FAIL loop 5|0' \
	    'vacuous-only|{"schema":"uvm-owner-race/1","loop":4,"what":"w","iterations_a":2,"iterations_b":9,"open_failed":7,"failed":false,"vacuous":true,"ok":false}|RE-RUN loop 4|1' \
	    'failed-only|{"schema":"uvm-owner-race/1","loop":3,"what":"w","iterations_a":9,"iterations_b":9,"open_failed":0,"failed":true,"vacuous":false,"ok":false}|FAIL loop 3|0' \
	    'vacuous-b-zero|{"schema":"uvm-owner-race/1","loop":4,"what":"w","iterations_a":9,"iterations_b":0,"open_failed":0,"failed":false,"vacuous":true,"ok":false}|second side did no work|1' \
	    'vacuous-loop5|{"schema":"uvm-owner-race/1","loop":5,"what":"w","iterations_a":9,"iterations_b":0,"open_failed":0,"failed":false,"vacuous":true,"ok":false}|could not be exercised|1' \
	    'clean|{"schema":"uvm-owner-race/1","loop":1,"what":"w","iterations_a":9,"iterations_b":9,"open_failed":0,"failed":false,"vacuous":false,"ok":true}|NOTHING|0'; do
		name=${fx%%|*}; rest=${fx#*|}; row=${rest%%|*}; rest=${rest#*|}; want=${rest%%|*}; wantrerun=${rest#*|}
		printf '%s\n' "$row" > "$T/rrr.jsonl"
		( rerun=0; out=; say() { out="$out$*;"; }; fail() { out="${out}FAIL $*;"; }
		  . "$T/rrr.sh"; read_race_rows "$T/rrr.jsonl"
		  case $want in NOTHING) [ -z "$out" ] || echo "fixture $name: said '$out'" ;; *) case $out in *"$want"*) ;; *) echo "fixture $name: said '$out', wanted $want" ;; esac ;; esac
		  [ "$rerun" = "$wantrerun" ] || echo "fixture $name: rerun $rerun, wanted $wantrerun"
		  case $name in failed-and-vacuous) case $out in *RE-RUN*) echo "fixture $name: a failed loop was read as RE-RUN" ;; esac ;; esac )
	done
	# the validation setting (RmValidateClientData) is read as a number, in the kenv and the sysctl form; a source that
	# cannot be read is RE-RUN, never "default".  Fixture: name|capture (printf format)|FAIL, NOFAIL or RERUN
	sed -n '/^# BEGIN validation_override/,/^# END validation_override/p' "$1" > "$T/vo.sh"
	[ -s "$T/vo.sh" ] || { echo "the boot script has no validation_override function"; return; }
	for fx in "all-2|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=0x2\"\\nsysctl exit 0\\n|FAIL:ALL" \
	    "handle-0x20|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=0x20\"\\nsysctl exit 0\\n|FAIL:HANDLE" \
	    "strict-0x80|kenv exit 0\\nsysctl exit 0\\nhw.nvidia.registry.dwords: RmValidateClientData=0x80\\n|FAIL:STRICT_CLIENT" \
	    "decimal-128|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"rmvalidateclientdata=128;RmFoo=0\"\\nsysctl exit 0\\n|FAIL:STRICT_CLIENT" \
	    "perkey-kenv|kenv exit 0\\nhw.nvidia.registry.RmValidateClientData=\"0x2\"\\nsysctl exit 0\\n|FAIL:ALL" \
	    "perkey-sysctl|kenv exit 0\\nsysctl exit 0\\nhw.nvidia.registry.RmValidateClientData: 2\\n|FAIL:ALL" \
	    "octal-0200|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=0200\"\\nsysctl exit 0\\n|FAIL:STRICT_CLIENT" \
	    "leading-zero-010|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=010\"\\nsysctl exit 0\\n|FAIL:ALL" \
	    "zero-default|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=0x0\"\\nsysctl exit 0\\n|NOFAIL" \
	    "0x11-on|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=0x11\"\\nsysctl exit 0\\n|NOFAIL" \
	    "1-on|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=1\"\\nsysctl exit 0\\n|NOFAIL" \
	    "handle-0x30-on|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=0x30\"\\nsysctl exit 0\\n|NOFAIL" \
	    "strict-0xC0-on|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=0xC0\"\\nsysctl exit 0\\n|NOFAIL" \
	    "all-3-on|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=3\"\\nsysctl exit 0\\n|NOFAIL" \
	    "leading-zeros-1|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=0001\"\\nsysctl exit 0\\n|NOFAIL" \
	    "absent|kenv exit 0\\nsysctl exit 0\\n|NOFAIL" \
	    "old-name-is-not-the-key|kenv exit 0\\nhw.nvidia.registry.RmClientDataValidation=\"0\"\\nsysctl exit 0\\n|NOFAIL" \
	    "longer-name-not-the-key|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"XRmValidateClientData=0x2\"\\nsysctl exit 0\\n|NOFAIL" \
	    "name-with-suffix-not-the-key|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientDataFoo=0x2\"\\nsysctl exit 0\\n|NOFAIL" \
	    "sysctl-node-missing|kenv exit 0\\nsysctl: unknown oid 'hw.nvidia.registry'\\nsysctl exit 1\\n|RERUN" \
	    "kenv-failed|kenv exit 1\\nsysctl exit 0\\n|RERUN" \
	    "empty-capture||RERUN" \
	    "key-without-value|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=\"\\nsysctl exit 0\\n|RERUN" \
	    "hex-no-digits|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=0x\"\\nsysctl exit 0\\n|RERUN" \
	    "hex-garbage|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=0xZZ\"\\nsysctl exit 0\\n|RERUN" \
	    "trailing-garbage|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=2abc\"\\nsysctl exit 0\\n|RERUN" \
	    "wide-hex|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=0x10000000000000002\"\\nsysctl exit 0\\n|RERUN" \
	    "wide-decimal|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=99999999999999999999\"\\nsysctl exit 0\\n|RERUN" \
	    "over-32-bits|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=4294967296\"\\nsysctl exit 0\\n|RERUN" \
	    "fail-beats-ok|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=0x1\"\\nsysctl exit 0\\nhw.nvidia.registry.dwords: RmValidateClientData=0x80\\n|FAIL:STRICT_CLIENT" \
	    "fail-beats-unreadable|kenv exit 0\\nhw.nvidia.registry.RegistryDwords=\"RmValidateClientData=\"\\nsysctl exit 0\\nhw.nvidia.registry.dwords: RmValidateClientData=0x2\\n|FAILMIX:ALL"; do
		name=${fx%%|*}; rest=${fx#*|}; cap=${rest%|*}; want=${rest##*|}
		printf "$cap" > "$T/vo.txt"
		( out=; rerun=0; say() { out="$out$*;"; }; fail() { out="${out}FAIL $*;"; }
		  . "$T/vo.sh"; validation_override "$T/vo.txt"
		  case $want in
		  FAIL:*|FAILMIX:*) fld=${want#*:}
		      case $out in *"field $fld)"*) ;; *) echo "validation fixture $name: said '$out', wanted FAIL in field $fld" ;; esac
		      case $out in FAIL*|*";FAIL"*) ;; *) echo "validation fixture $name: no FAIL: '$out'" ;; esac
		      case $want in FAIL:*) [ "$rerun" = 0 ] || echo "validation fixture $name: a plain FAIL also set RE-RUN: '$out'" ;; esac ;;
		  RERUN) [ "$rerun" = 1 ] || echo "validation fixture $name: not RE-RUN: '$out'"
		      case $out in *FAIL*|*"ok   "*) echo "validation fixture $name: RE-RUN fixture also said '$out'" ;; esac ;;
		  *) case $out in *FAIL*) echo "validation fixture $name: failed wrongly: '$out'" ;; esac
		      [ "$rerun" = 0 ] || echo "validation fixture $name: wanted no RE-RUN: '$out'" ;;
		  esac )
	done
}
if [ -z "$(boot_pins "$BOOT" "$REPO/tests/ioctl/uvm_owner_probe.c")" ]; then echo "ok   the boot script: refusal field, row reading on fixtures, RE-RUN, validation, busy device"; else echo "FAIL boot script:"; boot_pins "$BOOT" "$REPO/tests/ioctl/uvm_owner_probe.c"; rc=1; fi
boot_plant() {	# boot_plant <name> <sed>
	sed -e "$2" "$BOOT" > "$T/boot.mut"
	if cmp -s "$BOOT" "$T/boot.mut"; then echo "FAIL plant $1 changed nothing"; rc=1; return; fi
	if [ -z "$(boot_pins "$T/boot.mut" "$REPO/tests/ioctl/uvm_owner_probe.c")" ]; then echo "FAIL plant $1 stays green"; rc=1; else echo "ok   plant $1 is red"; fi
}
boot_plant no-rerun-verdict 's/say "RE-RUN"/say "FAIL"/'
boot_plant no-busy-guard 's/another process holds \/dev\/nvidiactl/something/'
boot_plant validation-ignored 's/turns RM client validation off/note/'
boot_plant validation-never-off 's/then echo [A-Z_]*$/then :/'
boot_plant validation-all-missing 's/; then echo ALL$/; then :/'
boot_plant validation-handle-missing 's/; then echo HANDLE$/; then :/'
boot_plant validation-strict-missing 's/; then echo STRICT_CLIENT$/; then :/'
boot_plant validation-handle-mask-wrong 's/(\$1 \& 0x30) == 0x20/(\$1 \& 0x20) == 0x20/'
boot_plant validation-strict-mask-wrong 's/(\$1 \& 0xC0) == 0x80/(\$1 \& 0x80) == 0x80/'
boot_plant validation-zero-is-off 's/(\$1 \& 0x3) == 0x2 ))/(\$1 \& 0x3) == 0x2 || \$1 == 0 ))/'
boot_plant unread-is-default '/^# BEGIN validation_override/,/^# END validation_override/s/rerun=1$/rerun=0/'
boot_plant old-key-name 's/RmValidateClientData\[\[:space:\]\]\*/RmClientDataValidation[[:space:]]*/'
boot_plant exits-not-read 's/if \[ "\$vsrc" != 0 \]; then/if false; then/'
boot_plant kenv-exit-not-read 's/if \[ "\$vkrc" != 0 \]; then/if false; then/'
boot_plant octal-reading-dropped 's/then vreadings="\$vreadings \$((vval))"; fi/then :; fi/'
boot_plant range-unchecked 's/\[ "\$vn" -gt 4294967295 \]/false/'
boot_plant name-boundary-dropped 's/(^|\[^A-Za-z0-9_\])RmValidateClientData/RmValidateClientData/'
boot_plant rerun-set-on-fail 's/^			vbad=1$/			vbad=1; rerun=1/'
boot_plant loop5-cause-generic 's/could not be exercised/did no work/'
boot_plant failed-arm-dead 's/"failed":true/"failed":nope/'
boot_plant stale-busy-check 's/fstat.race/fstat.before/g'
boot_plant vacuous-says-fail 's/say "RE-RUN loop/fail "loop/'
sed -e 's/\\"refusal\\"/\\"refused_x\\"/' "$REPO/tests/ioctl/uvm_owner_probe.c" > "$T/probe.mut3"
if [ -z "$(boot_pins "$BOOT" "$T/probe.mut3")" ]; then echo "FAIL plant probe-no-refusal-field stays green"; rc=1; else echo "ok   plant probe-no-refusal-field is red"; fi

# the version pins the man page promises: a live MODULE_DEPEND on exactly version 2 (uvm_main.c; nvidia.ko's MODULE_VERSION is pinned in nvidia-gb10-freebsd tests/client_owner_test.sh)
mkdir -p "$T/p-depend-1"; cp "$REPO"/sys/*.c "$REPO"/sys/*.h "$T/p-depend-1/"
sed -e 's/MODULE_DEPEND(nvidia_uvm, nvidia, 2, 2, 2);/MODULE_DEPEND(nvidia_uvm, nvidia, 1, 1, 1);/' "$REPO/sys/uvm_main.c" > "$T/p-depend-1/uvm_main.c"
if cmp -s "$REPO/sys/uvm_main.c" "$T/p-depend-1/uvm_main.c"; then echo "FAIL plant module-depend-version-1 changed nothing"; rc=1
elif [ -z "$(check_tree "$T/p-depend-1")" ]; then echo "FAIL plant module-depend-version-1 stays green"; rc=1; else echo "ok   plant module-depend-version-1 is red"; fi

# the man page says what a load failure looks like against an older nvidia.ko
if grep -qF 'mismatched dependency' "$REPO/man/nvidia-uvm.4" && grep -qF 'symbol nvUvmInterfaceClientHold undefined' "$REPO/man/nvidia-uvm.4"; then echo "ok   the man page names both load failures"; else echo "FAIL the man page lacks the mismatched dependency or the undefined symbol"; rc=1; fi
# the nvidia side of the version pin, when that repository is at hand (NVIDIA_GB10_REPO, or the default work tree)
NVREPO=${NVIDIA_GB10_REPO:-$WORK/nvidia-gb10-freebsd}
if [ -f "$NVREPO/patches/patch-zz-nvidia_client_owner_version_pci.c" ]; then
	if grep -qF '+MODULE_VERSION(nvidia, 2);' "$NVREPO/patches/patch-zz-nvidia_client_owner_version_pci.c"; then echo "ok   nvidia.ko is module version 2 ($NVREPO)"; else echo "FAIL $NVREPO does not make nvidia.ko module version 2"; rc=1; fi
else
	echo "note nvidia-gb10-freebsd not found at $NVREPO (set NVIDIA_GB10_REPO): the nvidia.ko version pin is checked by that repository's tests/client_owner_test.sh"
fi

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
