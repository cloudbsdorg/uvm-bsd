#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# Compiles tests/abi_initialize.c against the pinned NVIDIA headers.
# The compiler must be amd64. The pin must be tag 595.99.02, the same
# version the port reads from x11/nvidia-driver/Makefile.version, and
# must hash every header the compile includes. A different ioctl number
# and a wider rmStatus must fail to compile. An extra 4-byte field still
# fits in the tail padding, so the member list rejects it.

set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
pin=$root/third_party/open-gpu-kernel-modules
pinfile=$pin/PIN
uvm=$pin/kernel-open/nvidia-uvm
common=$pin/kernel-open/common/inc
src=$root/tests/abi_initialize.c
verlock=$root/tests/test_version_lock.sh

fail() {
	printf 'FAIL: %s\n' "$1" >&2
	exit 1
}

[ -f "$pinfile" ] || fail "pinned headers are missing: $pinfile"
[ -f "$src" ] || fail "missing $src"

tag=
commit=
hashed=
while read -r key value; do
	case $key in
	''|'#'*) continue ;;
	tag) tag=$value ;;
	commit) commit=$value ;;
	*)
		[ -n "$value" ] || fail "PIN line has no path: $key"
		case $value in
		*' '*|*'	'*|*..*) fail "PIN path is not one header: [$value]" ;;
		esac
		got=$(sha256sum "$pin/$value" | awk '{ print $1 }')
		[ "$got" = "$key" ] || fail "pinned file does not match PIN: $value"
		hashed="${hashed}${value}
"
		;;
	esac
done < "$pinfile"
[ "$tag" = "595.99.02" ] || fail "PIN tag is [$tag], want 595.99.02"
[ "$commit" = "e394a404108a9ce5bba988c1dddf694dabbd4a43" ] \
	|| fail "PIN commit is [$commit], want e394a404108a9ce5bba988c1dddf694dabbd4a43"
for path in \
	kernel-open/nvidia-uvm/uvm_linux_ioctl.h \
	kernel-open/nvidia-uvm/uvm_ioctl.h \
	kernel-open/nvidia-uvm/uvm_types.h \
	kernel-open/common/inc/cpuopsys.h \
	kernel-open/common/inc/nvCpuUuid.h \
	kernel-open/common/inc/nv_uvm_user_types.h \
	kernel-open/common/inc/nvlimits.h \
	kernel-open/common/inc/nvstatus.h \
	kernel-open/common/inc/nvstatuscodes.h \
	kernel-open/common/inc/nvtypes.h
do
	printf '%s' "$hashed" | grep -F -x -q "$path" || fail "PIN does not hash $path"
done

ver=$("$verlock") || fail "version lock failed"
printf '%s\n' "$ver" | grep -F -q "NVIDIA_DISTVERSION=$tag " \
	|| fail "driver version is not the pinned tag $tag"

command -v cc >/dev/null 2>&1 || fail "cc is not installed"
machine=$(cc -dumpmachine)
case $machine in
x86_64-*|amd64-*|x86_64|amd64) ;;
*) fail "cc targets [$machine]. This check is the amd64 layout of UVM_INITIALIZE." ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
transcript=

note() {
	transcript="${transcript}$1
"
}

compile() {
	# $1 is an optional directory that hides the pinned uvm header.
	_overlay=${1:-}
	_out=$tmp/abi
	if [ -n "$_overlay" ]; then
		_out=$tmp/abi-bad
		set -- -std=c11 -Wall -Wextra -Werror -I"$_overlay" -I"$uvm" -I"$common" -o "$_out" "$src"
	else
		set -- -std=c11 -Wall -Wextra -Werror -I"$uvm" -I"$common" -o "$_out" "$src"
	fi
	if cc "$@" 2>"$tmp/cc.err"; then
		return 0
	fi
	if [ -z "$_overlay" ]; then
		cat "$tmp/cc.err" >&2
	fi
	return 1
}

expect_fail() {
	_name=$1
	_need=$2
	_dir=$3
	_rc=0
	compile "$_dir" || _rc=$?
	if [ "$_rc" -eq 0 ]; then
		printf 'FAIL: %s compiled\n' "$_name" >&2
		exit 1
	fi
	if ! grep -F -q -- "$_need" "$tmp/cc.err"; then
		printf 'FAIL: %s did not report [%s]\n' "$_name" "$_need" >&2
		cat "$tmp/cc.err" >&2
		exit 1
	fi
	note "case $_name: compile failed"
}

bad=$tmp/bad-number
mkdir -p "$bad"
awk '{ gsub(/0x30000001/, "0x30000099"); print }' "$uvm/uvm_linux_ioctl.h" > "$bad/uvm_linux_ioctl.h"
grep -F -q '0x30000099' "$bad/uvm_linux_ioctl.h" || fail "mutation did not land: ioctl number"
if grep -F -q '0x30000001' "$bad/uvm_linux_ioctl.h"; then
	fail "mutation left the original ioctl number"
fi
expect_fail "wrong ioctl number" "UVM_INITIALIZE is not 0x30000001" "$bad"

# Members after the preprocessor, so a macro or a block comment cannot hide a field.
pre_members() {
	_overlay=${1:-}
	if [ -n "$_overlay" ]; then
		set -- -std=c11 -E -I"$_overlay" -I"$uvm" -I"$common" -
	else
		set -- -std=c11 -E -I"$uvm" -I"$common" -
	fi
	cc "$@" <<'EOF' | awk '
		{ line[NR] = $0 }
		END {
			end = 0
			for (i = 1; i <= NR; i++)
				if (line[i] ~ /^} UVM_INITIALIZE_PARAMS;$/) end = i
			if (end == 0) exit 2
			start = 0
			for (i = end; i >= 1; i--)
				if (line[i] ~ /^typedef struct$/) { start = i; break }
			if (start == 0) exit 2
			for (i = start + 1; i < end; i++) {
				s = line[i]
				gsub(/[[:space:]]+/, " ", s)
				sub(/^ /, "", s)
				sub(/ $/, "", s)
				if (s == "" || s == "{" || s == "}") continue
				print s
			}
		}
	'
#include "uvm_linux_ioctl.h"
EOF
}

members='NvU64 flags __attribute__ ((aligned (8)));
NV_STATUS rmStatus;'
got=$(pre_members) || fail "could not preprocess UVM_INITIALIZE_PARAMS"
[ "$got" = "$members" ] || fail "pinned UVM_INITIALIZE_PARAMS members are [$got]"

pad=$tmp/pad-field
mkdir -p "$pad"
awk '/^} UVM_INITIALIZE_PARAMS;/ { print "    NvU32     added;" } { print }' \
	"$uvm/uvm_linux_ioctl.h" > "$pad/uvm_linux_ioctl.h"
grep -F -q 'NvU32     added;' "$pad/uvm_linux_ioctl.h" || fail "mutation did not land: extra field"
if ! compile "$pad"; then
	printf 'FAIL: 4-byte field did not compile, so the member check was not the rejection\n' >&2
	cat "$tmp/cc.err" >&2
	exit 1
fi
got=$(pre_members "$pad") || fail "could not preprocess the extra-field copy"
[ "$got" != "$members" ] || fail "extra field was accepted as flags and rmStatus"
note "case extra 4-byte field: members rejected"

wide=$tmp/wide-status
mkdir -p "$wide"
sed 's/typedef NvU32 NV_STATUS;/typedef NvU64 NV_STATUS;/' "$common/nvstatus.h" > "$wide/nvstatus.h"
grep -F -q 'typedef NvU64 NV_STATUS;' "$wide/nvstatus.h" || fail "mutation did not land: wide NV_STATUS"
if grep -F -q 'typedef NvU32 NV_STATUS;' "$wide/nvstatus.h"; then
	fail "mutation left the 4-byte NV_STATUS"
fi
expect_fail "wide rmStatus" "rmStatus is not 4 bytes" "$wide"

compile
note "PASS: UVM_INITIALIZE is 0x30000001. UVM_INITIALIZE_PARAMS is 16 bytes, alignment 8, flags at 0, rmStatus is 4 bytes at 8. headers tag $tag commit $commit."
evidence=$root/tests/evidence/abi-initialize.txt
printf '%s' "$transcript" | cmp -s - "$evidence" \
	|| fail "evidence file does not match this run"
printf '%s' "$transcript"
