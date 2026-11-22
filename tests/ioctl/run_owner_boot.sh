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
#   - the validation setting of the host is read from nvidia-settings.txt: RmValidateClientData with
#     (v & 0x3) == 0x2, (v & 0x30) == 0x20 or (v & 0xC0) == 0x80 turns the run into FAIL, because the
#     refusals mean nothing then; 0 is the default (on); a source that cannot be read, or a value that is not
#     a valid base-0 number, ends RE-RUN (a FAIL stands whatever else could not be read).
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
# validation_override <settings file>: reads the RM registry key RmValidateClientData (nvrm_registry.h) from the
# capture of the host's kenv and hw.nvidia.registry sysctl (nvidia-settings.txt: "kenv exit N", the nvidia lines of
# kenv, the sysctl text, "sysctl exit N").  Outcomes:
#   - the key is present and validation is OFF: FAIL, whatever else could not be read.  Validation is off when
#     (v & 0x3) == 0x2 (field ALL), (v & 0x30) == 0x20 (field HANDLE) or (v & 0xC0) == 0x80 (field STRICT_CLIENT);
#     0 means the default (on);
#   - a source could not be read (a failed kenv or sysctl, a missing hw.nvidia.registry node because nvidia is
#     not loaded, an empty capture, a failed search of the capture) or a value is not a valid number: RE-RUN,
#     never "default";
#   - otherwise (key absent, or any other value): ok, a value is reported.
# Numbers are read base 0 as the driver's own parse does (see validation_number): 0x or 0X is hex, a leading 0 is
# octal (digits 0 to 7 only, so 028 is not a number), anything else is decimal; at most 32 bits.  The key name is
# matched as a whole token, case-insensitively.  The masks live only in validation_off_field.
validation_off_field() {	# validation_off_field <number>: prints the field that turns validation off, or nothing
	if [ $(( ($1 & 0x3) == 0x2 )) = 1 ]; then echo ALL
	elif [ $(( ($1 & 0x30) == 0x20 )) = 1 ]; then echo HANDLE
	elif [ $(( ($1 & 0xC0) == 0x80 )) = 1 ]; then echo STRICT_CLIENT
	fi
}
# validation_number <text>: prints the value of a base-0 number of at most 32 bits, or nothing.  The driver reads
# each RegistryDwords value with strtoul(value, NULL, 0) cast to 32 bits (nvidia_os_registry.c of the NVIDIA FreeBSD
# OS layer); this reader takes only whole numbers (0x hex, a leading 0 octal, else decimal; "0" is octal zero) and
# gives nothing for text strtoul would read a prefix of (028 reads as 2 there) or truncate (more than 32 bits).
validation_number() {
	if printf '%s\n' "$1" | grep -Eq '^0[xX][0-9a-fA-F]{1,16}$'; then vn=$(($1))
	elif printf '%s\n' "$1" | grep -Eq '^0[0-7]{0,40}$'; then vn=$(($1))
	elif printf '%s\n' "$1" | grep -Eq '^[1-9][0-9]{0,9}$'; then vn=$1
	else return 0
	fi
	if [ -n "$vn" ] && [ "$vn" -ge 0 ] && [ "$vn" -le 4294967295 ]; then echo "$vn"; fi
}
validation_override() {
	vkrc=$(sed -n 's/^kenv exit \([0-9][0-9]*\)$/\1/p' "$1" 2>/dev/null | head -1 || true)
	vsrc=$(sed -n 's/^sysctl exit \([0-9][0-9]*\)$/\1/p' "$1" 2>/dev/null | head -1 || true)
	if [ -z "$vkrc" ] || [ -z "$vsrc" ]; then
		say "RE-RUN the validation setting could not be read: the capture $1 is empty or lacks the kenv or sysctl exit line; see it and $(dirname "$1")/kenv.txt, then run again (an unreadable setting is not treated as the default)"
		rerun=1
		return
	fi
	vunread=
	if [ "$vsrc" != 0 ]; then
		vunread="sysctl hw.nvidia.registry failed (exit $vsrc): load nvidia and run again"
	elif [ "$vkrc" != 0 ]; then
		vunread="kenv failed (exit $vkrc, see kenv.txt): run as root and run again"
	fi
	vtok='(^|[^A-Za-z0-9_])RmValidateClientData[[:space:]]*[=:][[:space:]]*"?[^;"[:space:]]*'
	vgrc=0
	vlines=$(grep -aEi "$vtok" "$1" 2>/dev/null) || vgrc=$?
	if [ "$vgrc" -gt 1 ]; then
		say "RE-RUN the capture $1 could not be searched (grep exit $vgrc): check that it is readable and run again (an unreadable setting is not treated as the default)"
		rerun=1
		return
	fi
	vbad=0
	vunnum=0
	vnummsg=
	vfound=0
	if [ "$vgrc" = 0 ]; then
	while IFS= read -r vwhole; do
		vfrags=$(printf '%s\n' "$vwhole" | grep -aEio "$vtok" || true)
		while IFS= read -r vline; do
			[ -n "$vline" ] || continue
			vfound=1
			# the first strip removes the boundary character the token regex may have captured (a ":" or "="
			# glued to the key: fixtures colon-glued and equals-glued); then everything through the separator
			vval=$(printf '%s\n' "$vline" | sed -E 's/^[^A-Za-z]//; s/^[^=:]*[=:][[:space:]]*"?//')
			vnum=$(validation_number "$vval")
			if [ -z "$vnum" ]; then
				vnummsg="$vnummsg '$vval'"
				vunnum=1
				continue
			fi
			vfield=$(validation_off_field "$vnum")
			if [ -n "$vfield" ]; then
				vlow=$(printf '%s' "$vwhole" | tr 'A-Z' 'a-z')
				case $vlow in
				*registrydwords*|*registry.dwords*) vwhere="remove RmValidateClientData=... from RegistryDwords (hw.nvidia.registry.RegistryDwords in the loader.conf of the boot environment under test)" ;;
				*) vwhere="remove hw.nvidia.registry.RmValidateClientData from the loader tunables" ;;
				esac
				fail "RmValidateClientData $vval ($vnum) turns RM client validation off (field $vfield) in: $vwhole; $vwhere, reboot and run again; refusals are not meaningful on this host"
				vbad=1
			else
				say "     RmValidateClientData is set to $vval ($vnum): validation stays on (0 is the default); in: $vwhole"
			fi
		done <<EOF2
$vfrags
EOF2
	done <<EOF
$vlines
EOF
	fi
	# not read: say why, whatever else is decided (an operator must not learn it one rerun later); the RE-RUN
	# wording appears only in a run that ends RE-RUN
	if [ -n "$vnummsg" ]; then
		if [ "$vbad" = 0 ]; then
			say "RE-RUN RmValidateClientData value$vnummsg is not a valid number (hex 0x..., octal 0..., or decimal, at most 32 bits): correct it in the loader settings, reboot and run again (an unreadable setting is not treated as the default)"
		else
			say "     note: RmValidateClientData value$vnummsg is not a valid number"
		fi
	fi
	if [ -n "$vunread" ] && [ "$vbad" = 0 ]; then
		say "RE-RUN the validation setting could not be read: $vunread (not read is not default)"
		rerun=1
	elif [ -n "$vunread" ]; then
		say "     note: $vunread"
	fi
	[ "$vbad" = 0 ] || return 0		# a FAIL stands whatever else could not be read
	if [ "$vgrc" = 0 ] && [ "$vfound" = 0 ]; then
		# the search matched and no value was parsed (a binary capture, or the two searches disagree): not read
		say "RE-RUN the capture matched RmValidateClientData but no value could be parsed from it: not read is not default"
		rerun=1
		return 0
	fi
	if [ "$vunnum" != 0 ]; then rerun=1; return 0; fi
	[ -z "$vunread" ] || return 0
	if [ "$vfound" = 0 ]; then
		say "ok   no RmValidateClientData in kenv or sysctl (default: validation on); see nvidia-settings.txt"
	else
		say "ok   RmValidateClientData does not turn validation off"
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
{ kenv > "$OUT/kenv.txt" 2>&1; echo "kenv exit $?"; grep -i nvidia "$OUT/kenv.txt"; sysctl hw.nvidia.registry 2>&1; echo "sysctl exit $?"; } > "$OUT/nvidia-settings.txt" 2>&1
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
