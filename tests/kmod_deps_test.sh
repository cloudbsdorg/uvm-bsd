#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# kmod_deps_test.sh - the static load check of the two UVM modules
# (Track #2444).  No kldload(8), no root:
#
#   (a) nvidia-uvm.ko declares exactly {kernel, nvidia}: the native
#       driver depends on no Linux module.  The set is compared exactly,
#       not by membership: a regression that puts MODULE_DEPEND(nvidia_uvm,
#       linux64elf) back would pass a membership check, and the module
#       would then drag the Linuxulator into every native boot.
#   (b) nvidia-uvm.ko imports no Linux symbol: every undefined symbol of
#       the module is checked for a linux_ prefix.
#   (c) nvidia_uvm_linux.ko declares exactly {kernel, the 64-bit
#       Linuxulator, linux_common, nvidia_uvm}: the bridge belongs to the
#       Linuxulator and needs the driver.
#   (d) every undefined symbol of a module resolves in the kernel or in
#       the file of one of its declared dependencies, and only against
#       their GLOBAL defined symbols: the kernel linker resolves a
#       module's undefined symbols only from the kernel and from the
#       module's DIRECT dependencies (linker_file_lookup_symbol() does not
#       follow a dependency's own dependencies), and never against a local
#       symbol.  The symbols nvidia-uvm.ko takes from nvidia.ko (the
#       nvUvmInterface* entry points) are the one exception: nvidia.ko
#       lives in another repository and is not built here, so they are
#       listed and not resolved.
#   (e) the driver exports uvm_bridge_fd_is_ours to the bridge: a module
#       symbol is exported only when the module's Makefile names it in
#       EXPORT_SYMS, and without that the kernel linker refuses the
#       bridge with "symbol uvm_bridge_fd_is_ours undefined".
#
# Falsifiability: every check is a function that takes its input as an
# argument, and each is run once on a planted input it must refuse.  A
# check that cannot fail is decoration.
#
# Usage: tests/kmod_deps_test.sh [REPO]
#   REPO  the checkout to check; defaults to the parent of this script.

set -eu

PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
LC_ALL=C
export PATH LC_ALL

TESTSDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=${1:-$(CDPATH= cd -- "$TESTSDIR/.." && pwd)}
WORK=${GB10_WORK:-$REPO/tmp}
KO=$REPO/sys/nvidia-uvm.ko
BRKO=$REPO/linux/nvidia_uvm_linux.ko

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

[ -f "$KO" ] || fail "no $KO: run make build first"

mkdir -p "$WORK"
T=$(mktemp -d "$WORK/kmod-deps-test.XXXXXXXX")
trap 'rm -rf "$T"' EXIT HUP INT QUIT TERM

# deps_of KO MODULE: the dependency names MODULE_DEPEND(9) embeds in the
# module.  Each MODULE_DEPEND(m, dep, ...) emits a linker-set pointer
# symbol __set_modmetadata_set_sym__mod_metadata_<m>_depend_on_<dep> and a
# static struct _<m>_depend_on_<dep> (sys/module.h), so the names are read
# from the symbols.
deps_of() {
	nm "$1" 2>/dev/null | grep "_$2_depend_on_" | awk '{print $NF}' \
	    | sed 's/.*_depend_on_//' | sort -u
}

# The 64-bit Linuxulator exports the MODULE_VERSION interface linux64elf on
# arm64 and linux64 on amd64; the file is linux64.ko on both.
case $(uname -p) in
arm64|aarch64) LINUX_DEP=linux64elf ;;
amd64) LINUX_DEP=linux64 ;;
*) fail "unknown architecture $(uname -p)" ;;
esac

# check_exact_deps LABEL WANT-FILE ACTUAL-FILE: the two files must hold the
# same set of names.
check_exact_deps() {
	_label=$1
	_want=$2
	_got=$3
	if ! cmp -s "$_want" "$_got"; then
		echo "    wanted: $(paste -s -d ' ' "$_want")"
		echo "    got:    $(paste -s -d ' ' "$_got")"
		fail "$_label"
	fi
	echo "ok   $_label: exactly {$(paste -s -d ' ' "$_got")}"
}

# ---- (a) the native driver depends on no Linux module ----
deps_of "$KO" nvidia_uvm > "$T/native-deps"
[ -s "$T/native-deps" ] || fail "no MODULE_DEPEND symbols found in $KO"
printf 'kernel\nnvidia\n' > "$T/native-deps-want"
check_exact_deps "nvidia-uvm.ko declares no Linuxulator dependency" \
    "$T/native-deps-want" "$T/native-deps"
if grep -qE '^(linux|linux64|linux64elf|linux_common)$' "$T/native-deps"; then
	fail "nvidia-uvm.ko depends on the Linuxulator: the native driver must not"
fi

# ---- (b) the native driver imports no Linux symbol ----
nm -u "$KO" > "$T/native-undef" || fail "nm -u failed on $KO"
awk '{print $NF}' < "$T/native-undef" | sort -u > "$T/native-undef-names"
if grep -q '^linux' "$T/native-undef-names"; then
	grep '^linux' "$T/native-undef-names" | sed 's/^/    Linux symbol: /' >&2
	fail "nvidia-uvm.ko imports a Linux symbol"
fi
echo "ok   nvidia-uvm.ko imports no Linux symbol"

# ---- (c) the bridge belongs to the Linuxulator and depends on the driver ----
[ -f "$BRKO" ] || fail "no $BRKO: run make build first"
deps_of "$BRKO" nvidia_uvm_linux > "$T/bridge-deps"
[ -s "$T/bridge-deps" ] || fail "no MODULE_DEPEND symbols found in $BRKO"
printf 'kernel\n%s\nlinux_common\nnvidia_uvm\n' "$LINUX_DEP" > "$T/bridge-deps-want"
check_exact_deps "nvidia_uvm_linux.ko declares the Linuxulator and the driver" \
    "$T/bridge-deps-want" "$T/bridge-deps"

# ---- (d) every undefined symbol resolves ----
KERNEL=$(/sbin/sysctl -n kern.bootfile)
KDIR=$(dirname "$KERNEL")
[ -f "$KERNEL" ] || fail "cannot read the running kernel $KERNEL"

# defined_syms FILE OUT: the GLOBAL defined symbol names of FILE.  The
# kernel linker never resolves a module's undefined symbols against a
# local symbol, so only the global ones count.  nm runs into a file
# first: in a pipe an nm failure would be read as "no symbols".
defined_syms() {
	nm "$1" 2>/dev/null | awk 'NF == 3 && ($2 ~ /^[A-TV-Z]$/ || $2 == "i") {print $3}' >> "$2"
}

: > "$T/defined"
defined_syms "$KERNEL" "$T/defined"
for f in "$KDIR/linux64.ko" "$KDIR/linux_common.ko" "$KO"; do
	[ -f "$f" ] || fail "no $f: the module's declared dependency has no file"
	defined_syms "$f" "$T/defined"
done
sort -u "$T/defined" -o "$T/defined"

# check_resolved LABEL UNDEF-FILE: every name in UNDEF-FILE must be in the
# defined set.
check_resolved() {
	_label=$1
	_undef=$2
	comm -23 "$_undef" "$T/defined" > "$T/unresolved"
	if [ -s "$T/unresolved" ]; then
		sed 's/^/    unresolved symbol: /' "$T/unresolved" >&2
		fail "$_label"
	fi
	echo "ok   $_label"
}

nm -u "$BRKO" 2>/dev/null | awk '{print $NF}' | sort -u > "$T/bridge-undef"
check_resolved "every undefined symbol of nvidia_uvm_linux.ko resolves" \
    "$T/bridge-undef"

# The driver's own undefined symbols: the nvUvmInterface* entry points come
# from nvidia.ko, which is built in another repository and is not here.
# Every other symbol must resolve in the kernel.
grep '^nvUvmInterface' "$T/native-undef-names" > "$T/native-foreign"
grep -v '^nvUvmInterface' "$T/native-undef-names" > "$T/native-own"
check_resolved "every undefined symbol of nvidia-uvm.ko but the nvidia.ko ones resolves" \
    "$T/native-own"
echo "ok   nvidia-uvm.ko takes $(wc -l < "$T/native-foreign" | tr -d ' ') symbols from nvidia.ko (not built here)"

# ---- (e) the driver exports the bridge's entry point ----
nm "$KO" 2>/dev/null | awk 'NF == 3 && ($2 ~ /^[A-TV-Z]$/ || $2 == "i") {print $3}' \
    | grep -qx 'uvm_bridge_fd_is_ours' ||
    fail "nvidia-uvm.ko does not export uvm_bridge_fd_is_ours to the bridge (add it to EXPORT_SYMS in sys/Makefile)"
echo "ok   nvidia-uvm.ko exports uvm_bridge_fd_is_ours"

# ---- the controls: each check must refuse a planted input ----
# The controls run the checks in a subshell: a check that refuses calls
# fail, which exits, and the subshell keeps that exit from ending the run.
# (a-control) a dependency list with a Linuxulator dependency in it.
printf 'kernel\nlinux64elf\nnvidia\n' > "$T/ctl-deps"
printf 'kernel\nnvidia\n' > "$T/ctl-deps-want"
if (check_exact_deps "control" "$T/ctl-deps-want" "$T/ctl-deps") >/dev/null 2>&1; then
	fail "the dependency check accepts a planted Linuxulator dependency"
fi
echo "ok   the dependency check refuses a planted Linuxulator dependency"

# (b-control) a Linux symbol among the undefined ones.
printf 'linux_ioctl_register_handler\n' > "$T/ctl-undef-names"
if grep -q '^linux' "$T/ctl-undef-names"; then
	echo "ok   the Linux symbol check refuses a planted Linux symbol"
else
	fail "the Linux symbol check does not see a planted Linux symbol"
fi

# (d-control) an undefined symbol that nothing resolves.
printf 'uvm_a_symbol_that_nothing_defines\n' > "$T/ctl-undef"
if (check_resolved "control" "$T/ctl-undef") >/dev/null 2>&1; then
	fail "the resolution check accepts an undefined symbol"
fi
echo "ok   the resolution check refuses an undefined symbol"

echo "PASS"
