#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# run_owner_boot.sh - the test-boot recipe of Track #2373 (self-recording; run as
# root in the test BE, with nvidia.ko and nvidia-uvm.ko of the fixed build loaded):
#   1. nvidia-uvm.ko is loaded (it links only when nvidia.ko exports every symbol it imports,
#      the two client hold exports included); with NVIDIA_KO=/path/to/nvidia.ko the exports are
#      also listed with nm
#   2. the two-user probe (JSON lines): every row must hold
#   3. the five race loops, SECONDS each (default 60)
#   4. afterwards: no thread in wait channel nvcobusy, no ownership bug line on the
#      console, no nvidiactl file left open by a dead process, nvidia-uvm unloads
#      and nvidia unloads
#   5. the CUDA vecadd regression (proves libcuda's allocations are decoded and
#      recorded): CUDA_VECADD=/path/to/run-linux-cuda.sh, skipped with a FAIL line when unset
# How to read the result:
#   - a probe row that is not ok, or a loop that failed ("failed":true), is NO-GO; a negative row
#     ("expected":"refused") must carry "refusal":true (status 0x1B, NV_ERR_INSUFFICIENT_PERMISSIONS) and a
#     positive owner row ("expected":"not refused") must not; a probe with no positive row proves nothing;
#   - a loop is vacuous when it did no work ("vacuous":true): a run whose only problems are vacuous loops
#     ends RE-RUN, not PASS and not FAIL; a loop that failed AND was vacuous is a failure (loop 4 is also
#     vacuous when its open failures reach half of its attempts; loop 5 when none of the stranger's frees
#     met a client);
#   - the race loops are not started while another process holds /dev/nvidiactl (fstat.race, taken just
#     before them: loop 5 frees handles next to the victim's): the run ends RE-RUN;
#   - the validation setting of the host is read from nvidia-settings.txt: RmClientDataValidation overridden
#     to 0 turns the run into FAIL, because the refusals mean nothing then.
# Results: $GB10_WORK/logs/uvm-2373-boot/<time>/ ; the last line is PASS, FAIL or RE-RUN.
#
# Usage: sh tests/ioctl/run_owner_boot.sh [SECONDS]

set -u
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
export PATH
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/../.." && pwd)
WORK=${GB10_WORK:?set GB10_WORK (the work root; nothing is written elsewhere)}
SECS=${1:-60}
OUT=$WORK/logs/uvm-2373-boot/$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$OUT" "$WORK/tmp"
rc=0
rerun=0
say() { echo "$*" | tee -a "$OUT/result.txt"; }
fail() { say "FAIL $*"; rc=1; }
# BEGIN validation_override
# validation_override <settings file>: fails when RmClientDataValidation is set to 0 (kenv or sysctl form, the
# value read as a number), says so when it is set otherwise, and says "no override" when it is absent
validation_override() {
	vline=$(grep -i 'RmClientDataValidation' "$1" | head -1)
	if [ -z "$vline" ]; then
		say "ok   no override of RmClientDataValidation in kenv or sysctl (default: on); see nvidia-settings.txt"
		return
	fi
	vval=$(printf '%s\n' "$vline" | sed -En 's/.*RmClientDataValidation[=: "]*(0x[0-9a-fA-F]+|[0-9]+).*/\1/p')
	if [ -n "$vval" ] && [ $((vval)) = 0 ]; then
		fail "RmClientDataValidation is overridden to off ($vline): refusals are not meaningful on this host"
	else
		say "     RmClientDataValidation is set ($vline): read nvidia-settings.txt; it is not set to 0"
	fi
}
# END validation_override
# BEGIN read_race_rows
# read_race_rows <race.jsonl>: a failed loop is a failure whether or not it also did little; a loop that
# only did little is RE-RUN (rerun=1), with the cause read from its own numbers
read_race_rows() {
	for n in 1 2 3 4 5; do
		row=$(grep "\"loop\":$n," "$1" | head -1)
		[ -n "$row" ] || continue
		what=$(printf '%s\n' "$row" | sed -n 's/.*"what":"\([^"]*\)".*/\1/p')
		ia=$(printf '%s\n' "$row" | sed -n 's/.*"iterations_a":\([0-9]*\),.*/\1/p')
		ib=$(printf '%s\n' "$row" | sed -n 's/.*"iterations_b":\([0-9]*\),.*/\1/p')
		of=$(printf '%s\n' "$row" | sed -n 's/.*"open_failed":\([0-9]*\),.*/\1/p')
		case $row in
		*'"failed":true'*) fail "loop $n failed ($what): see race.err for the reason" ;;
		*'"vacuous":true'*)
			if [ "${ia:-0}" = 0 ]; then cause="the first side did no work (iterations_a 0)"
			elif [ "${ib:-0}" = 0 ] && [ "$n" = 5 ]; then cause="no stranger free met a client (iterations_b 0): loop 5 could not be exercised on this RM, perhaps because client handles are not handed out in sequence; the other loops' results stand"
			elif [ "${ib:-0}" = 0 ]; then cause="the second side did no work (iterations_b 0)"
			else cause="the opens failed ($of failures against $ia attempts): free descriptors and run again"; fi
			say "RE-RUN loop $n was vacuous ($what): $cause"; rerun=1 ;;
		esac
	done
}
# END read_race_rows

if [ "$(id -u)" != 0 ]; then echo "run as root" >&2; exit 2; fi
T=$(mktemp -d "$WORK/tmp/owner-boot.XXXXXXXX") || exit 2
trap 'rm -rf "$T"' EXIT

dmesg > "$OUT/dmesg.before" 2>&1
cc -std=c11 -D_DEFAULT_SOURCE -I "$REPO/sys" -o "$T/probe" "$REPO/tests/ioctl/uvm_owner_probe.c" || fail "cannot build the probe"
cc -std=c11 -D_DEFAULT_SOURCE -I "$REPO/sys" -o "$T/race" "$REPO/tests/ioctl/uvm_owner_race.c" -lpthread || fail "cannot build the race loops"

# 1. exports (nvidia-uvm.ko links only when nvidia.ko exports every symbol it imports)
kldstat > "$OUT/kldstat.txt" 2>&1
grep -q 'nvidia-uvm' "$OUT/kldstat.txt" && say "ok   nvidia-uvm is loaded (so its undefined exports resolved)" || fail "nvidia-uvm is not loaded"
if [ -n "${NVIDIA_KO:-}" ]; then
	nm -D "$NVIDIA_KO" 2>/dev/null | grep -E ' T nvUvmInterface' > "$OUT/exports.txt"
	grep -q 'nvUvmInterfaceClientHold' "$OUT/exports.txt" && grep -q 'nvUvmInterfaceClientRelease' "$OUT/exports.txt" \
	    && say "ok   $NVIDIA_KO exports $(wc -l < "$OUT/exports.txt" | tr -d ' ') nvUvmInterface symbols, Hold and Release among them" \
	    || fail "$NVIDIA_KO does not export the client hold pair"
fi
fstat 2>/dev/null | grep nvidiactl > "$OUT/fstat.before" || true

# 2. the probe
"$T/probe" -j > "$OUT/probe.jsonl" 2>&1; prc=$?
[ "$prc" = 0 ] && say "ok   probe exit 0" || fail "probe exit $prc"
say "     probe rows: $(grep -c '"row"' "$OUT/probe.jsonl"), not ok: $(grep -c '"ok":false' "$OUT/probe.jsonl"), summaries: $(grep -c summary "$OUT/probe.jsonl")"
neg=$(grep -c '"expected":"refused"' "$OUT/probe.jsonl"); pos=$(grep -c '"expected":"not refused"' "$OUT/probe.jsonl")
neg_bad=$(grep '"expected":"refused"' "$OUT/probe.jsonl" | grep -vc '"refusal":true'); pos_bad=$(grep '"expected":"not refused"' "$OUT/probe.jsonl" | grep -c '"refusal":true')
say "     negative rows: $neg (not 0x1B: $neg_bad), positive owner rows: $pos (refused with 0x1B: $pos_bad)"
[ "$neg" -gt 0 ] || fail "no negative rows: the probe did not run its refusal rows"
[ "$pos" -gt 0 ] || fail "no positive owner rows: a probe with no positive row proves nothing (a module that refuses everyone would pass)"
[ "$neg_bad" = 0 ] || { fail "$neg_bad negative rows were not refused with 0x1B (status 27): see probe.jsonl"; grep '"expected":"refused"' "$OUT/probe.jsonl" | grep -v '"refusal":true' | head -5 | while read -r l; do say "     $l"; done; }
[ "$pos_bad" = 0 ] || { fail "$pos_bad owner rows were refused with 0x1B (status 27): the module refuses its owner"; grep '"expected":"not refused"' "$OUT/probe.jsonl" | grep '"refusal":true' | head -5 | while read -r l; do say "     $l"; done; }
[ "$(grep -c '"ok":false' "$OUT/probe.jsonl")" = 0 ] || fail "the probe has a row that is not ok"
{ kenv > "$OUT/kenv.txt" 2>&1; echo "kenv exit $?"; grep -i nvidia "$OUT/kenv.txt"; sysctl hw.nvidia 2>&1; echo "sysctl exit $?"; } > "$OUT/nvidia-settings.txt" 2>&1
validation_override "$OUT/nvidia-settings.txt"
grep '"ok":false' "$OUT/probe.jsonl" | head -5 | while read -r l; do say "     $l"; done

# 3. the race loops
fstat 2>/dev/null | grep nvidiactl > "$OUT/fstat.race" || true	# taken now: a process may have opened the device since the start
if [ -s "$OUT/fstat.race" ]; then
	say "RE-RUN another process holds /dev/nvidiactl: $(awk '{ print $2 "[" $3 "]" }' "$OUT/fstat.race" | sort -u | tr '\n' ' ')(fstat.race). The race loops free handles next to the victim's and are not started: stop those processes and run again"
	rerun=1
else
	"$T/race" -j -t "$SECS" > "$OUT/race.jsonl" 2> "$OUT/race.err"; rrc=$?
	cat "$OUT/race.jsonl" "$OUT/race.err" >> "$OUT/result.txt"
	loops=$(grep -c '"schema":"uvm-owner-race/1"' "$OUT/race.jsonl")
	[ "$loops" = 5 ] && say "ok   five loops reported" || fail "$loops loops reported, expected 5 (exit $rrc; race.err has the reason)"
	read_race_rows "$OUT/race.jsonl"
fi

# 4. leftovers
if ps -axo pid,wchan,comm | grep -q '[n]vcobusy'; then fail "a thread sleeps in nvcobusy"; else say "ok   no thread in nvcobusy"; fi
dmesg > "$OUT/dmesg.after" 2>&1
diff "$OUT/dmesg.before" "$OUT/dmesg.after" | sed -n 's/^> //p' > "$OUT/dmesg.new"
grep 'client ownership' "$OUT/dmesg.new" > "$OUT/refusal-lines.txt" || true
say "     rate-limited refusal lines from the probe (expected): $(wc -l < "$OUT/refusal-lines.txt" | tr -d ' ') (see refusal-lines.txt)"
if grep -E -f "$HERE/owner_bug_patterns.txt" "$OUT/dmesg.new" > "$OUT/bug-lines.txt"; then fail "the glue logged a bug line (bug-lines.txt)"; else say "ok   no bug line from the glue"; fi
fstat 2>/dev/null | grep nvidiactl > "$OUT/fstat.after" || true
b=$(wc -l < "$OUT/fstat.before" | tr -d ' '); a=$(wc -l < "$OUT/fstat.after" | tr -d ' ')
if [ "$a" -le "$b" ]; then say "ok   nvidiactl open files: $b before, $a after (a dead process leaves none)"; else fail "nvidiactl open files grew from $b to $a: a file leaked"; fi
kldunload nvidia-uvm > "$OUT/unload-uvm.txt" 2>&1 && say "ok   kldunload nvidia-uvm" || fail "kldunload nvidia-uvm: $(cat "$OUT/unload-uvm.txt")"
kldunload nvidia > "$OUT/unload-nvidia.txt" 2>&1 && say "ok   kldunload nvidia" || say "     kldunload nvidia refused (other modules hold it; the order of a full unload is nvidia-drm, nvidia-modeset, nvidia): $(cat "$OUT/unload-nvidia.txt")"

# 5. CUDA regression (kldload nvidia-uvm again first, if it was unloaded)
kldload nvidia-uvm > /dev/null 2>&1 || true
if [ -n "${CUDA_VECADD:-}" ]; then
	sh "$CUDA_VECADD" > "$OUT/cuda.txt" 2>&1 && say "ok   CUDA vecadd" || fail "CUDA vecadd (see cuda.txt)"
else
	fail "CUDA_VECADD is not set: the vecadd regression did not run"
fi
if [ "$rc" != 0 ]; then say "FAIL"; exit "$rc"; fi
if [ "$rerun" != 0 ]; then say "RE-RUN"; exit 3; fi
say "PASS"
exit 0
