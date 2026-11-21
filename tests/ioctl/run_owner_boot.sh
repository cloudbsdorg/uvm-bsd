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
#   3. the four race loops, SECONDS each (default 60)
#   4. afterwards: no thread in wait channel nvcobusy, no ownership bug line on the
#      console, no nvidiactl file left open by a dead process, nvidia-uvm unloads
#      and nvidia unloads
#   5. the CUDA vecadd regression (proves libcuda's allocations are decoded and
#      recorded): CUDA_VECADD=/path/to/run-linux-cuda.sh, skipped with a FAIL line when unset
# Results: $GB10_WORK/logs/uvm-2373-boot/<time>/ ; the last line is PASS or FAIL.
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
say() { echo "$*" | tee -a "$OUT/result.txt"; }
fail() { say "FAIL $*"; rc=1; }

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
grep '"ok":false' "$OUT/probe.jsonl" | head -5 | while read -r l; do say "     $l"; done

# 3. the race loops
"$T/race" -j -t "$SECS" > "$OUT/race.jsonl" 2>&1; rrc=$?
[ "$rrc" = 0 ] && say "ok   race loops exit 0" || fail "race loops exit $rrc"
cat "$OUT/race.jsonl" >> "$OUT/result.txt"

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
[ "$rc" = 0 ] && say "PASS" || say "FAIL"
exit "$rc"
