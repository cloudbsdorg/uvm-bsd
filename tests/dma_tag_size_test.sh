#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# dma_tag_size_test.sh - source-level check (Track #2295): a busdma tag
# must not be larger than what its allocations need.  busdma's
# bus_dmamem_alloc allocates the tag's maxsize, so a 64 MiB tag behind a
# 4 KiB page-table allocation became a 64 MiB contiguous allocation, which
# held Giant for minutes.  Rules, per bus_dma_tag_create() in sys/*.c:
#   - maxsize and maxsegsz must be PAGE_SIZE, UVM_GPU_DMA_MAXSIZE, or the
#     variable `bytes` (a pool sized by its own request);
#   - no numeric literal or shift in maxsize.
# And uvm_gpu_sysmem_alloc must refuse requests above UVM_GPU_DMA_MAXSIZE,
# which uvm.h defines as PAGE_SIZE.  The checker is itself tested against
# a bad fixture so that it can fail.  Run: sh tests/dma_tag_size_test.sh
set -eu
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
export PATH LC_ALL=C
TESTSDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$TESTSDIR/.." && pwd)
WORK=${GB10_WORK:-$HOME/work/gb10}
mkdir -p "$WORK/tmp"
T=$(mktemp -d "$WORK/tmp/uvm-dmasz.XXXXXXXX")
trap 'rm -rf "$T"' EXIT HUP INT QUIT TERM

# check_file FILE: print one line per offending bus_dma_tag_create call
check_file() {
	awk '
	function trim(s) { gsub(/^[ \t\n]+|[ \t\n]+$/, "", s); return s }
	{ src = src $0 "\n" }
	END {
		s = src
		while ((p = index(s, "bus_dma_tag_create(")) > 0) {
			s = substr(s, p + length("bus_dma_tag_create("))
			depth = 1; n = 1; arg[1] = ""
			for (i = 1; i <= length(s) && depth > 0; i++) {
				c = substr(s, i, 1)
				if (c == "(") depth++
				else if (c == ")") { depth--; if (depth == 0) break }
				if (c == "," && depth == 1) { n++; arg[n] = ""; continue }
				arg[n] = arg[n] c
			}
			ms = trim(arg[8]); sg = trim(arg[10])
			ok = 1
			if (ms !~ /^(PAGE_SIZE|UVM_GPU_DMA_MAXSIZE|bytes)$/) ok = 0
			if (sg !~ /^(PAGE_SIZE|UVM_GPU_DMA_MAXSIZE|bytes)$/) ok = 0
			if (!ok) printf("maxsize=[%s] maxsegsz=[%s]\n", ms, sg)
			s = substr(s, i)
		}
	}' "$1"
}

rc=0
# self-test: a 64 MiB tag must be reported, a right-sized one must not
cat > "$T/bad.c" <<'EOT'
error = bus_dma_tag_create(parent, PAGE_SIZE, 0, lim, BUS_SPACE_MAXADDR, NULL, NULL, 64u << 20, 1, 64u << 20, 0, NULL, NULL, &t);
EOT
cat > "$T/good.c" <<'EOT'
error = bus_dma_tag_create(parent, PAGE_SIZE, 0, lim, BUS_SPACE_MAXADDR, NULL, NULL, UVM_GPU_DMA_MAXSIZE, 1, UVM_GPU_DMA_MAXSIZE, 0, NULL, NULL, &t);
EOT
[ -n "$(check_file "$T/bad.c")" ] || { echo "FAIL checker accepts a 64 MiB tag"; rc=1; }
[ -z "$(check_file "$T/good.c")" ] || { echo "FAIL checker rejects a right-sized tag"; rc=1; }

for f in "$REPO"/sys/*.c; do
	out=$(check_file "$f")
	if [ -n "$out" ]; then
		echo "FAIL $(basename "$f"): oversized DMA tag: $out"
		rc=1
	fi
done
grep -Eq '^#define[[:space:]]+UVM_GPU_DMA_MAXSIZE[[:space:]]+PAGE_SIZE' "$REPO/sys/uvm.h" || {
	echo "FAIL uvm.h: UVM_GPU_DMA_MAXSIZE is not PAGE_SIZE"; rc=1; }
grep -q 'bytes > UVM_GPU_DMA_MAXSIZE' "$REPO/sys/uvm_gpu.c" || {
	echo "FAIL uvm_gpu.c: uvm_gpu_sysmem_alloc does not refuse oversize requests"; rc=1; }
[ $rc -eq 0 ] && echo "ok   dma_tag_size_test"
exit $rc
