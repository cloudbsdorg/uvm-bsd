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
# which uvm_pt_loop.h defines as PAGE_SIZE (its behaviour is tested by
# tests/unit/pt_loop_test.c).  The checker is itself tested against
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

SYS=${1:-$REPO/sys}
. "$REPO/tests/pins_lib.sh"
PIN_TMP=$T
PIN_N=0

pin_dma() {
	for f in "$1"/*.c; do
		[ -z "$(check_file "$f")" ] || { echo "oversized DMA tag in $f"; return 1; }
	done
	_h=$(pin_strip "$1/uvm_pt_loop.h")
	pin_dead "$_h" || return 1
	pin_has "$_h" '^#define[[:space:]]+UVM_GPU_DMA_MAXSIZE[[:space:]]+PAGE_SIZE$' || return 1
	pin_has "$_h" 'return \(bytes != 0 && bytes <= UVM_GPU_DMA_MAXSIZE\);' || return 1
	_b=$(pin_fn "$1/uvm_gpu.c" uvm_gpu_sysmem_alloc)
	pin_dead "$_b" || return 1
	pin_has "$_b" '^[[:space:]]*if \(!uvm_sysmem_size_ok\(bytes\)\)$' || return 1
	pin_has "$_b" '^[[:space:]]*return \(NV_ERR_INVALID_ARGUMENT\);' || return 1
	# the request is never rewritten before it is used
	! printf '%s\n' "$_b" | grep -Eq '(^|[^=!<>])[[:space:]]bytes[[:space:]]*=[^=]'
}
pin_dma "$SYS" || { echo "FAIL pin dma does not hold on $SYS"; rc=1; }

# planted defects
pin_mutate "tag back to 64 MiB" pin_dma uvm_gpu.c 's|NULL, NULL, UVM_GPU_DMA_MAXSIZE, 1, UVM_GPU_DMA_MAXSIZE, 0,|NULL, NULL, 64u << 20, 1, 64u << 20, 0,|'
pin_mutate "tag maxsize doubled" pin_dma uvm_gpu.c 's|NULL, NULL, UVM_GPU_DMA_MAXSIZE, 1,|NULL, NULL, UVM_GPU_DMA_MAXSIZE * 2, 1,|'
pin_mutate "constant raised" pin_dma uvm_pt_loop.h 's|^#define	UVM_GPU_DMA_MAXSIZE	PAGE_SIZE|#define	UVM_GPU_DMA_MAXSIZE	(PAGE_SIZE * 2)|'
pin_mutate "size check removed" pin_dma uvm_gpu.c 's|if (!uvm_sysmem_size_ok(bytes))|if (0 \&\& !uvm_sysmem_size_ok(bytes))|'
pin_mutate "size check commented out" pin_dma uvm_gpu.c 's|^\([[:space:]]*\)if (!uvm_sysmem_size_ok(bytes))|\1/* if (!uvm_sysmem_size_ok(bytes)) */|'
pin_mutate "request rewritten" pin_dma uvm_gpu.c 's|^\([[:space:]]*\)error = bus_dmamem_alloc(gpu->dmat, kva|\1bytes = 64u << 20;\n&|'
pin_mutate "size rule loosened" pin_dma uvm_pt_loop.h 's|bytes <= UVM_GPU_DMA_MAXSIZE|bytes <= UVM_GPU_DMA_MAXSIZE * 2|'
pin_mutate "size rule allows zero" pin_dma uvm_pt_loop.h 's|bytes != 0 \&\& ||'

[ $rc -eq 0 ] && echo "ok   dma_tag_size_test"
exit $rc
