#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# nm the 595.99.02 FreeBSD nv-kernel.o and decide how nvidia-uvm.ko links.
# The object is not in git. Point NV_KERNEL_O at src/nvidia/nv-kernel.o from
# NVIDIA-FreeBSD-x86_64-595.99.02.tar.xz. This script does not run make and
# does not build nvidia.ko.
#
# nvUvmInterface* must not be defined. Every rm_gpu_ops_* name in the
# NV_UVM_ENABLE arm of the pinned nv_uvm_interface.c must be a global text
# symbol in that object. The upstream src/nvidia/Makefile sets no
# EXPORT_SYMS. The script reads /home/mlapointe/git/freebsd-ports. That
# path is fixed. x11/nvidia-kmod applies
# x11/nvidia-driver/files/extra-patch-src-Makefile-2.in. That patch's
# added lines must be one EXPORT_SYMS assignment of the eight nv_kthread
# names and nothing else. FreeBSD kmod.mk localizes every global the
# patch leaves off the list. The measured plan is to compile the .c with
# NV_UVM_ENABLE into nvidia-uvm.ko and add the names to that patch.

set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
pin=$root/third_party/open-gpu-kernel-modules
pinfile=$pin/PIN
iface=$pin/kernel-open/nvidia/nv_uvm_interface.c
ports=/home/mlapointe/git/freebsd-ports
export_patch=$ports/x11/nvidia-driver/files/extra-patch-src-Makefile-2.in
evidence=$root/tests/evidence/symbol-gate.txt

want_iface=ca600c4265bc549bfac7a1225b98fe691e1825660ed510665c95f146af775179
want_object=b64589a3760bd2acea31ca104181ad98c1af3fdc588d251c2d1bdb55a8ccdc54
want_makefile=6377487037ce79c8369bbc7e45fae6595003eced40a750c0d3bf97b9ebf5c939
want_patch=cade8991717382e96c1b6f95151cd4f487f29f119927e32f159fee0f63775b13
want_sources=29e058f616b0c0db861e6ace15a5bb55403a30da6f114db1b4ba17938e892da9
want_cflags=c398b57b1de33cb76d870560e2a2b8079cba3485dbc6149e65989275bbb52795
want_calls=76

fail() {
	printf 'FAIL: %s\n' "$1" >&2
	exit 1
}

[ -f "$pinfile" ] || fail "PIN is missing: $pinfile. Restore third_party/open-gpu-kernel-modules/PIN from this repository."
[ -f "$iface" ] || fail "pinned nv_uvm_interface.c is missing: $iface. Restore kernel-open/nvidia/nv_uvm_interface.c from this repository."
grep -F -x -q \
	"$want_iface  kernel-open/nvidia/nv_uvm_interface.c" \
	"$pinfile" || fail "PIN does not record nv_uvm_interface.c. Restore the committed PIN line for that file."

obj=${NV_KERNEL_O:-}
[ -n "$obj" ] || fail "NV_KERNEL_O is unset. Set it to src/nvidia/nv-kernel.o from NVIDIA-FreeBSD-x86_64-595.99.02.tar.xz"
[ -f "$obj" ] || fail "NV_KERNEL_O is not a file: $obj. Point it at src/nvidia/nv-kernel.o from NVIDIA-FreeBSD-x86_64-595.99.02.tar.xz"
kmod_mk=$(dirname "$obj")/Makefile
[ -f "$kmod_mk" ] || fail "src/nvidia/Makefile is not beside NV_KERNEL_O. Extract the driver archive so Makefile sits next to nv-kernel.o"

ports_real=$(realpath -e "$ports") || fail "freebsd-ports tree is missing at /home/mlapointe/git/freebsd-ports. Clone the FreeBSD ports tree at that path. The path is fixed."
[ "$ports_real" = "/home/mlapointe/git/freebsd-ports" ] \
	|| fail "freebsd-ports resolved to $ports_real. This test reads /home/mlapointe/git/freebsd-ports and does not take another path."
patch_real=$(realpath -e "$export_patch") || fail "missing export patch $export_patch. Use the FreeBSD ports tree at /home/mlapointe/git/freebsd-ports."
case $patch_real in
"$ports_real"/x11/nvidia-driver/files/extra-patch-src-Makefile-2.in) ;;
*) fail "export patch is not x11/nvidia-driver/files/extra-patch-src-Makefile-2.in: $patch_real" ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
transcript=
# Hash the copies, then nm and grep only those copies.
cp "$iface" "$tmp/nv_uvm_interface.c"
cp "$obj" "$tmp/nv-kernel.o"
cp "$kmod_mk" "$tmp/kmod-makefile"
iface=$tmp/nv_uvm_interface.c
obj=$tmp/nv-kernel.o
got_iface=$(sha256sum "$iface" | awk '{ print $1 }')
[ "$got_iface" = "$want_iface" ] || fail "nv_uvm_interface.c hash is $got_iface, expected $want_iface. Restore the committed 595.99.02 file."
got_object=$(sha256sum "$obj" | awk '{ print $1 }')
[ "$got_object" = "$want_object" ] || fail "nv-kernel.o hash is $got_object, expected $want_object from driver 595.99.02. Re-extract NVIDIA-FreeBSD-x86_64-595.99.02.tar.xz and point NV_KERNEL_O at src/nvidia/nv-kernel.o."
got_makefile=$(sha256sum "$tmp/kmod-makefile" | awk '{ print $1 }')
[ "$got_makefile" = "$want_makefile" ] || fail "src/nvidia/Makefile hash is $got_makefile, expected $want_makefile. Re-extract NVIDIA-FreeBSD-x86_64-595.99.02.tar.xz so Makefile sits next to nv-kernel.o."
comm -23 /dev/null /dev/null >/dev/null || fail "comm is not usable"

note() {
	transcript="${transcript}$1
"
}

calls_of() {
	# $1 is the interface source. Identifiers only; the hash above is what
	# binds this to the pinned file.
	grep -oE 'rm_gpu_ops_[A-Za-z0-9_]+' "$1" | sort -u
}

# $1 is nm output. Prints defined rm_gpu_ops_* names.
# Undefined lines are "U name" (two fields). Defined lines are "addr type name".
# mode "text" keeps type T. mode "other" keeps every other defined type.
nm_names() {
	_mode=$1
	_file=$2
	awk -v mode="$_mode" '
		$1 == "U" { next }
		$3 ~ /^rm_gpu_ops_[A-Za-z0-9_]+$/ {
			if (mode == "text" && $2 == "T") print $3
			if (mode == "other" && $2 != "T") print $3
		}
	' "$_file" | sort -u
}

# Exit 0 when a defined symbol name starts with nvUvmInterface.
# Exit 1 when none does. Any other status means nm text could not be read.
defines_uvm() {
	awk '
		$1 == "U" { next }
		$3 ~ /^nvUvmInterface/ { found = 1 }
		END { exit found ? 0 : 1 }
	' "$1"
}

# $1 nm text (any nm invocation). $2 one name per line.
same_set() {
	_nm=$1
	_calls=$2
	_drc=0
	defines_uvm "$_nm" || _drc=$?
	if [ "$_drc" -eq 0 ]; then
		printf 'blob contains nvUvmInterface\n' >&2
		return 1
	fi
	if [ "$_drc" -ne 1 ]; then
		printf 'could not read nm output\n' >&2
		return 1
	fi
	nm_names text "$_nm" > "$tmp/text.txt"
	nm_names other "$_nm" > "$tmp/other.txt"
	sort -u "$_calls" > "$tmp/calls.txt"
	if [ ! -s "$tmp/calls.txt" ]; then
		printf 'interface calls no rm_gpu_ops_ symbols\n' >&2
		return 1
	fi
	if ! missing=$(comm -23 "$tmp/calls.txt" "$tmp/text.txt"); then
		printf 'comm failed while comparing symbols\n' >&2
		return 1
	fi
	if ! extra=$(comm -13 "$tmp/calls.txt" "$tmp/text.txt"); then
		printf 'comm failed while comparing symbols\n' >&2
		return 1
	fi
	if ! extra_other=$(comm -13 "$tmp/calls.txt" "$tmp/other.txt"); then
		printf 'comm failed while comparing symbols\n' >&2
		return 1
	fi
	if [ -n "$missing" ]; then
		printf '%s\n' "$missing" | sort -u > "$tmp/missing.txt"
		if ! overlap=$(comm -12 "$tmp/other.txt" "$tmp/missing.txt"); then
			printf 'comm failed while comparing symbols\n' >&2
			return 1
		fi
		if [ -n "$overlap" ]; then
			printf 'called rm_gpu_ops_ symbol is not global text\n' >&2
			return 1
		fi
		printf 'blob is missing a called rm_gpu_ops_ symbol\n' >&2
		return 1
	fi
	if [ -n "$extra" ] || [ -n "$extra_other" ]; then
		printf 'blob defines an rm_gpu_ops_ symbol the interface does not call\n' >&2
		return 1
	fi
	return 0
}

expect_reject() {
	_name=$1
	_need=$2
	_nm=$3
	_calls=$4
	_rc=0
	same_set "$_nm" "$_calls" 2>"$tmp/why" || _rc=$?
	if [ "$_rc" -eq 0 ]; then
		printf 'FAIL: %s was accepted\n' "$_name" >&2
		exit 1
	fi
	if ! grep -F -q -- "$_need" "$tmp/why"; then
		printf 'FAIL: %s did not report [%s]\n' "$_name" "$_need" >&2
		cat "$tmp/why" >&2
		exit 1
	fi
	note "case $_name: rejected"
}

calls_of "$iface" > "$tmp/calls"
call_n=$(awk 'END { print NR }' "$tmp/calls")
[ "$call_n" -eq "$want_calls" ] || fail "interface call count is $call_n, want $want_calls"
# The #else arm returns NV_OK and must not contain these calls.
awk '
	/^[[:space:]]*#[[:space:]]*if[[:space:]]+defined\(NV_UVM_ENABLE\)/ {
		depth++
		if (uvm == 0) { uvm = depth; arm = 1 }
		next
	}
	/^[[:space:]]*#[[:space:]]*if/ { depth++; next }
	/^[[:space:]]*#[[:space:]]*else/ { if (depth == uvm) arm = 0; next }
	/^[[:space:]]*#[[:space:]]*endif/ {
		if (depth == uvm) { uvm = 0; arm = 0 }
		if (depth > 0) depth--
		next
	}
	/rm_gpu_ops_/ { if (arm) inn++; else out++ }
	END { print inn+0, out+0 }
' "$iface" > "$tmp/arms"
read -r in_arm out_arm < "$tmp/arms"
[ "$in_arm" -eq "$want_calls" ] || fail "NV_UVM_ENABLE arm has $in_arm rm_gpu_ops_ names, want $want_calls"
[ "$out_arm" -eq 0 ] || fail "rm_gpu_ops_ names sit outside the NV_UVM_ENABLE arm"

nm -g "$obj" > "$tmp/nm-g.txt" || fail "nm -g failed"
nm "$obj" > "$tmp/nm-all.txt" || fail "nm failed"
[ -s "$tmp/nm-g.txt" ] || fail "nm -g produced no output"
[ -s "$tmp/nm-all.txt" ] || fail "nm produced no output"
_rc=0
defines_uvm "$tmp/nm-all.txt" || _rc=$?
if [ "$_rc" -eq 0 ]; then
	fail "blob contains nvUvmInterface"
fi
if [ "$_rc" -ne 1 ]; then
	fail "could not read nm output"
fi
same_set "$tmp/nm-g.txt" "$tmp/calls" || fail "pinned object does not match the interface calls"

# One deleted global must be rejected. One invented UVM symbol must be
# rejected. One extra call must be rejected.
awk 'BEGIN { n = 0 }
	$2 == "T" && $3 ~ /^rm_gpu_ops_/ { n++; if (n == 1) next }
	{ print }' "$tmp/nm-g.txt" > "$tmp/nm-dropped.txt"
expect_reject "dropped blob symbol" \
	"blob is missing a called rm_gpu_ops_ symbol" \
	"$tmp/nm-dropped.txt" "$tmp/calls"

cp "$tmp/nm-all.txt" "$tmp/nm-grew.txt"
printf '0000000000000000 T nvUvmInterfaceFake\n' >> "$tmp/nm-grew.txt"
expect_reject "blob grew nvUvmInterface" \
	"blob contains nvUvmInterface" \
	"$tmp/nm-grew.txt" "$tmp/calls"

cp "$tmp/calls" "$tmp/calls-extra"
printf 'rm_gpu_ops_not_in_blob\n' >> "$tmp/calls-extra"
expect_reject "interface grew a call" \
	"blob is missing a called rm_gpu_ops_ symbol" \
	"$tmp/nm-g.txt" "$tmp/calls-extra"

cp "$tmp/nm-g.txt" "$tmp/nm-extra-rm.txt"
printf '0000000000000000 T rm_gpu_ops_not_called\n' >> "$tmp/nm-extra-rm.txt"
expect_reject "blob grew an uncalled symbol" \
	"blob defines an rm_gpu_ops_ symbol the interface does not call" \
	"$tmp/nm-extra-rm.txt" "$tmp/calls"
cp "$tmp/nm-g.txt" "$tmp/nm-data.txt"
printf '0000000000000000 D rm_gpu_ops_data_symbol\n' >> "$tmp/nm-data.txt"
expect_reject "blob grew a global data symbol" \
	"blob defines an rm_gpu_ops_ symbol the interface does not call" \
	"$tmp/nm-data.txt" "$tmp/calls"

awk 'BEGIN { n = 0 }
	$2 == "T" && $3 ~ /^rm_gpu_ops_/ && n == 0 { n = 1; $2 = "D"; print; next }
	{ print }
	END { if (n != 1) exit 1 }' "$tmp/nm-g.txt" > "$tmp/nm-textdata.txt" \
	|| fail "text-to-data mutation did not apply"
before_d=$(grep -c ' D rm_gpu_ops_' "$tmp/nm-g.txt" || true)
after_d=$(grep -c ' D rm_gpu_ops_' "$tmp/nm-textdata.txt" || true)
[ "$after_d" -eq $((before_d + 1)) ] || fail "text-to-data mutation did not apply"
expect_reject "called symbol changed from text to data" \
	"called rm_gpu_ops_ symbol is not global text" \
	"$tmp/nm-textdata.txt" "$tmp/calls"

cp "$tmp/nm-g.txt" "$tmp/nm-undef.txt"
printf '                 U nvUvmInterfaceRegisterGpu\n' >> "$tmp/nm-undef.txt"
grep -F -q 'U nvUvmInterfaceRegisterGpu' "$tmp/nm-undef.txt" \
	|| fail "undefined nvUvmInterface mutation did not apply"
if ! same_set "$tmp/nm-undef.txt" "$tmp/calls" 2>"$tmp/why"; then
	printf 'FAIL: undefined nvUvmInterface reference was rejected\n' >&2
	cat "$tmp/why" >&2
	exit 1
fi
cp "$tmp/nm-all.txt" "$tmp/nm-all-undef.txt"
printf '                 U nvUvmInterfaceRegisterGpu\n' >> "$tmp/nm-all-undef.txt"
_rc=0
defines_uvm "$tmp/nm-all-undef.txt" || _rc=$?
if [ "$_rc" -eq 0 ]; then
	fail "undefined nvUvmInterface reference was treated as a definition"
fi
if [ "$_rc" -ne 1 ]; then
	fail "could not read nm output"
fi
note "case undefined nvUvmInterface reference: accepted"

# Stock export list. kmod_syms.awk localizes every global left off it.
# Empty quotes are make concatenation. Strip them before looking for a name.
# A leftover quote, dollar, or backtick on an added line is still an expansion.
# Exit 0 when the file contains either forbidden name.
# Exit 1 when neither name is present. Exit 2 when grep could not read it.
names_present() {
	_rc=0
	grep -F -q 'rm_gpu_ops_' "$1" || _rc=$?
	if [ "$_rc" -eq 0 ]; then
		return 0
	fi
	if [ "$_rc" -ne 1 ]; then
		printf 'could not read the export patch (grep status %s)\n' "$_rc" >&2
		return 2
	fi
	_rc=0
	grep -F -q 'nvUvmInterface' "$1" || _rc=$?
	if [ "$_rc" -eq 0 ]; then
		return 0
	fi
	if [ "$_rc" -ne 1 ]; then
		printf 'could not read the export patch (grep status %s)\n' "$_rc" >&2
		return 2
	fi
	return 1
}

# Join added diff lines. A trailing odd backslash must continue onto
# another added line. A blank added line is ignored.
join_added() {
	if ! awk '
		function die() {
			print "added export line continues onto a non-added line" > "/dev/stderr"
			err = 1
			exit 1
		}
		function flush(text) {
			if (text ~ /[^[:space:]]/) print text
		}
		/^\+\+\+/ {
			if (cont) die()
			next
		}
		/^\+/ {
			body = substr($0, 2)
			n = 0
			t = body
			while (sub(/\\$/, "", t)) n++
			if (cont) buf = buf t
			else buf = t
			if (n % 2 == 1) { cont = 1; next }
			flush(buf)
			buf = ""
			cont = 0
			next
		}
		{
			if (cont) die()
		}
		END {
			if (err) exit 1
			if (cont) {
				print "added export line continues onto a non-added line" > "/dev/stderr"
				exit 1
			}
		}
	' "$1" > "$2"; then
		return 1
	fi
	return 0
}

check_exports() {
	sed "s/''//g" "$1" > "$tmp/patch-unfolded"
	_rc=0
	names_present "$tmp/patch-unfolded" || _rc=$?
	if [ "$_rc" -eq 0 ]; then
		printf 'stock EXPORT_SYMS already names an rm_gpu_ops_ or nvUvmInterface symbol\n' >&2
		return 1
	fi
	if [ "$_rc" -ne 1 ]; then
		return 1
	fi
	if ! join_added "$tmp/patch-unfolded" "$tmp/export-norm"; then
		return 1
	fi
	_rc=0
	names_present "$tmp/export-norm" || _rc=$?
	if [ "$_rc" -eq 0 ]; then
		printf 'stock EXPORT_SYMS already names an rm_gpu_ops_ or nvUvmInterface symbol\n' >&2
		return 1
	fi
	if [ "$_rc" -ne 1 ]; then
		return 1
	fi
	sq=$(printf '%s' "'")
	dq=$(printf '%s' '"')
	_rc=0
	grep -F -q '$' "$tmp/export-norm" || _rc=$?
	if [ "$_rc" -eq 0 ]; then
		printf 'added export line still has a make expansion\n' >&2
		return 1
	fi
	if [ "$_rc" -ne 1 ]; then
		printf 'could not read the export patch (grep status %s)\n' "$_rc" >&2
		return 1
	fi
	for meta in '`' "$sq" "$dq" '!='; do
		_rc=0
		grep -F -q "$meta" "$tmp/export-norm" || _rc=$?
		if [ "$_rc" -eq 0 ]; then
			case $meta in
			'!=')
				printf 'added export line uses a shell assignment\n' >&2
				;;
			*)
				printf 'added export line still has a make expansion\n' >&2
				;;
			esac
			return 1
		fi
		if [ "$_rc" -ne 1 ]; then
			printf 'could not read the export patch (grep status %s)\n' "$_rc" >&2
			return 1
		fi
	done
	for inc in '.include' '.sinclude'; do
		_rc=0
		grep -F -q "$inc" "$tmp/export-norm" || _rc=$?
		if [ "$_rc" -eq 0 ]; then
			printf 'added export line includes another makefile\n' >&2
			return 1
		fi
		if [ "$_rc" -ne 1 ]; then
			printf 'could not read the export patch (grep status %s)\n' "$_rc" >&2
			return 1
		fi
	done
	nlines=$(awk 'NF > 0 { c++ } END { print c+0 }' "$tmp/export-norm") || {
		printf 'could not read the export patch\n' >&2
		return 1
	}
	if [ "$nlines" -eq 0 ]; then
		printf 'added lines do not assign EXPORT_SYMS\n' >&2
		return 1
	fi
	if [ "$nlines" -ne 1 ]; then
		printf 'added export line is not a literal symbol list\n' >&2
		return 1
	fi
	_rc=0
	awk '
		BEGIN { status = 0 }
		NF == 0 { next }
		{
			if ($0 !~ /^[[:space:]]*EXPORT_SYMS[[:space:]]*(\+?=|:=)/) {
				status = 2
				exit
			}
			sub(/^[[:space:]]*EXPORT_SYMS[[:space:]]*(\+?=|:=)[[:space:]]*/, "")
			if ($0 ~ /[^A-Za-z0-9_[:space:]]/) {
				status = 3
				exit
			}
			n = split($0, f, /[[:space:]]+/)
			for (i = 1; i <= n; i++) if (f[i] != "") print f[i]
		}
		END { exit status }
	' "$tmp/export-norm" > "$tmp/export-words" || _rc=$?
	case $_rc in
	0) ;;
	2)
		printf 'added lines do not assign EXPORT_SYMS\n' >&2
		return 1
		;;
	3)
		printf 'added export line is not a literal symbol list\n' >&2
		return 1
		;;
	*)
		printf 'could not parse the export patch (awk status %s)\n' "$_rc" >&2
		return 1
		;;
	esac
	sort -u "$tmp/export-words" > "$tmp/export-got"
	printf '%s\n' \
		nv_kthread_q_flush \
		nv_kthread_q_init \
		nv_kthread_q_init_on_node \
		nv_kthread_q_item_init \
		nv_kthread_q_run_self_test \
		nv_kthread_q_schedule_q_item \
		nv_kthread_q_stop \
		nv_kthread_task_callback \
		| sort -u > "$tmp/kthread-want"
	if ! missing=$(comm -23 "$tmp/kthread-want" "$tmp/export-got"); then
		printf 'comm failed while comparing symbols\n' >&2
		return 1
	fi
	if ! extra=$(comm -13 "$tmp/kthread-want" "$tmp/export-got"); then
		printf 'comm failed while comparing symbols\n' >&2
		return 1
	fi
	if [ -n "$missing" ]; then
		printf '%s\n' "$missing" | while IFS= read -r sym; do
			[ -n "$sym" ] || continue
			printf 'export patch parse missed %s\n' "$sym" >&2
		done
		return 1
	fi
	if [ -n "$extra" ]; then
		printf 'export patch lists a symbol other than the kthread set\n' >&2
		return 1
	fi
	gotn=$(awk 'END { print NR+0 }' "$tmp/export-got") || {
		printf 'could not read the export patch\n' >&2
		return 1
	}
	if [ "$gotn" -ne 8 ]; then
		printf 'export patch parse missed a kthread symbol\n' >&2
		return 1
	fi
	return 0
}

# Join makefile backslash continuations. Strip comments first so a
# comment cannot satisfy an assignment check.
join_make_lines() {
	awk '
		{
			line = $0
			sub(/[[:space:]]*#.*/, "", line)
			n = 0
			t = line
			while (sub(/\\$/, "", t)) n++
			if (cont) buf = buf t
			else buf = t
			if (n % 2 == 1) { cont = 1; next }
			sub(/[[:space:]]+$/, "", buf)
			print buf
			buf = ""
			cont = 0
		}
		END {
			if (cont) {
				sub(/[[:space:]]+$/, "", buf)
				print buf
			}
		}
	' "$1"
}

check_upstream_makefile() {
	if ! join_make_lines "$1" > "$tmp/make-joined"; then
		printf 'could not read src/nvidia/Makefile\n' >&2
		return 1
	fi
	_rc=0
	grep -F -q 'EXPORT_SYMS' "$tmp/make-joined" || _rc=$?
	if [ "$_rc" -eq 0 ]; then
		printf 'src/nvidia/Makefile sets EXPORT_SYMS\n' >&2
		return 1
	fi
	if [ "$_rc" -ne 1 ]; then
		printf 'could not read src/nvidia/Makefile (grep status %s)\n' "$_rc" >&2
		return 1
	fi
	_rc=0
	awk '
		BEGIN { status = 0 }
		/^[[:space:]]*\.include[[:space:]]/ || /^[[:space:]]*\.sinclude[[:space:]]/ {
			if ($0 != ".include <bsd.kmod.mk>") status = 2
		}
		/^[[:space:]]*include[[:space:]]/ {
			if ($0 != "include nvidia-sources.mk" && $0 != "include ../common/optional-cflags.mk")
				status = 2
		}
		$0 == ".include <bsd.kmod.mk>" { seen = 1 }
		END {
			if (status) exit 2
			if (!seen) exit 3
			exit 0
		}
	' "$tmp/make-joined" >/dev/null || _rc=$?
	case $_rc in
	0) ;;
	2)
		printf 'src/nvidia/Makefile includes a file this check does not read\n' >&2
		return 1
		;;
	3)
		printf 'src/nvidia/Makefile does not include bsd.kmod.mk\n' >&2
		return 1
		;;
	*)
		printf 'could not read src/nvidia/Makefile (awk status %s)\n' "$_rc" >&2
		return 1
		;;
	esac
	return 0
}

# $1 is a makefile copied into $tmp. Reject EXPORT_SYMS and any include.
# The caller hashes the real driver files before this runs on them.
scan_included() {
	_label=$(basename "$1")
	if ! join_make_lines "$1" > "$tmp/inc-joined"; then
		printf 'could not read %s\n' "$_label" >&2
		return 1
	fi
	_irc=0
	grep -F -q 'EXPORT_SYMS' "$tmp/inc-joined" || _irc=$?
	if [ "$_irc" -eq 0 ]; then
		printf '%s sets EXPORT_SYMS\n' "$_label" >&2
		return 1
	fi
	if [ "$_irc" -ne 1 ]; then
		printf 'could not read %s (grep status %s)\n' "$_label" "$_irc" >&2
		return 1
	fi
	_irc=0
	awk '
		BEGIN { found = 0 }
		/^[[:space:]]*include[[:space:]]/ || /^[[:space:]]*\.include[[:space:]]/ || /^[[:space:]]*\.sinclude[[:space:]]/ { found = 1 }
		END { exit found ? 0 : 1 }
	' "$tmp/inc-joined" || _irc=$?
	case $_irc in
	0)
		printf '%s includes a file this check does not read\n' "$_label" >&2
		return 1
		;;
	1) ;;
	*)
		printf 'could not read %s (awk status %s)\n' "$_label" "$_irc" >&2
		return 1
		;;
	esac
	return 0
}

# $1 is src/nvidia/Makefile. The two includes it names must sit beside it.
check_sibling_includes() {
	_mk_dir=$(dirname "$1")
	_mk_dir=$(realpath -e "$_mk_dir") || {
		printf 'could not read the directory of src/nvidia/Makefile\n' >&2
		return 1
	}
	_sources=$_mk_dir/nvidia-sources.mk
	if [ ! -f "$_sources" ]; then
		printf 'nvidia-sources.mk is not beside the Makefile. Extract the driver archive so nvidia-sources.mk is present.\n' >&2
		return 1
	fi
	_common=$_mk_dir/../common
	_common=$(realpath -e "$_common") || {
		printf 'src/common is not beside src/nvidia. Extract the driver archive so optional-cflags.mk is present.\n' >&2
		return 1
	}
	_cflags=$_common/optional-cflags.mk
	if [ ! -f "$_cflags" ]; then
		printf 'optional-cflags.mk is missing. Extract the driver archive so src/common/optional-cflags.mk is present.\n' >&2
		return 1
	fi
	_sources=$(realpath -e "$_sources") || {
		printf 'could not read nvidia-sources.mk\n' >&2
		return 1
	}
	_cflags=$(realpath -e "$_cflags") || {
		printf 'could not read optional-cflags.mk\n' >&2
		return 1
	}
	if [ "$(dirname "$_sources")" != "$_mk_dir" ]; then
		printf 'nvidia-sources.mk is not in the Makefile directory\n' >&2
		return 1
	fi
	if [ "$(dirname "$_cflags")" != "$_common" ]; then
		printf 'optional-cflags.mk is not in src/common\n' >&2
		return 1
	fi
	cp "$_sources" "$tmp/nvidia-sources.mk"
	cp "$_cflags" "$tmp/optional-cflags.mk"
	_got=$(sha256sum "$tmp/nvidia-sources.mk" | awk '{ print $1 }')
	if [ "$_got" != "$want_sources" ]; then
		printf 'nvidia-sources.mk hash is %s, expected %s from driver 595.99.02. Re-extract NVIDIA-FreeBSD-x86_64-595.99.02.tar.xz.\n' "$_got" "$want_sources" >&2
		return 1
	fi
	_got=$(sha256sum "$tmp/optional-cflags.mk" | awk '{ print $1 }')
	if [ "$_got" != "$want_cflags" ]; then
		printf 'optional-cflags.mk hash is %s, expected %s from driver 595.99.02. Re-extract NVIDIA-FreeBSD-x86_64-595.99.02.tar.xz.\n' "$_got" "$want_cflags" >&2
		return 1
	fi
	scan_included "$tmp/nvidia-sources.mk" || return 1
	scan_included "$tmp/optional-cflags.mk" || return 1
	return 0
}

check_kmod_port() {
	if ! join_make_lines "$1" > "$tmp/port-joined"; then
		printf 'could not read x11/nvidia-kmod/Makefile\n' >&2
		return 1
	fi
	if ! awk '
		{
			line = $0
			sub(/^[[:space:]]+/, "", line)
			sub(/[[:space:]]+$/, "", line)
			if (line ~ /^FILESDIR[[:space:]]*(\+?=|:=|\?=|!=)/) filesdir = line
			if (line ~ /^PATCHDIR[[:space:]]*(\+?=|:=|\?=|!=)/) patchdir = line
		}
		END {
			print filesdir
			print patchdir
		}
	' "$tmp/port-joined" > "$tmp/port-assign"; then
		printf 'could not read x11/nvidia-kmod/Makefile\n' >&2
		return 1
	fi
	filesdir=$(awk 'NR==1 { print; exit }' "$tmp/port-assign")
	patchdir=$(awk 'NR==2 { print; exit }' "$tmp/port-assign")
	want_filesdir=$(printf 'FILESDIR=\t${.CURDIR}/../nvidia-driver/files')
	want_patchdir=$(printf 'PATCHDIR=\t${FILESDIR}')
	if [ "$filesdir" != "$want_filesdir" ]; then
		printf 'x11/nvidia-kmod does not take files from nvidia-driver/files\n' >&2
		return 1
	fi
	if [ "$patchdir" != "$want_patchdir" ]; then
		printf 'x11/nvidia-kmod PATCHDIR is not FILESDIR\n' >&2
		return 1
	fi
	if ! awk '
		BEGIN { found = 0 }
		{
			if ($0 ~ /^[[:space:]]*SUB_PATCHES[[:space:]]*(\+?=|:=)/) {
				n = split($0, f, /[[:space:]]+/)
				for (i = 1; i <= n; i++)
					if (f[i] == "extra-patch-src-Makefile-2") found = 1
			}
		}
		END { exit found ? 0 : 1 }
	' "$tmp/port-joined"; then
		printf 'x11/nvidia-kmod does not apply extra-patch-src-Makefile-2\n' >&2
		return 1
	fi
	if ! awk '
		BEGIN { pre = 0; fors = 0; sedline = 0; extra = 0 }
		/^pre-patch:/ { pre = 1 }
		$0 == ".for p in ${SUB_PATCHES}" { fors++ }
		index($0, "@${SED} '\''s/%%NVSRC%%/${NVSRC}/g'\'' ${PATCHDIR}/${p}.in > ${WRKDIR}/${p}") { sedline = 1 }
		$0 ~ /^EXTRA_PATCHES[[:space:]]*\+=[[:space:]]*\$\{WRKDIR\}\/\$\{p\}[[:space:]]*$/ { extra = 1 }
		END { exit (pre && fors >= 2 && sedline && extra) ? 0 : 1 }
	' "$tmp/port-joined"; then
		printf 'x11/nvidia-kmod does not apply patches from PATCHDIR\n' >&2
		return 1
	fi
	_krc=0
	grep -F -q '!=' "$tmp/port-joined" || _krc=$?
	if [ "$_krc" -eq 0 ]; then
		printf 'x11/nvidia-kmod/Makefile uses a shell assignment\n' >&2
		return 1
	fi
	if [ "$_krc" -ne 1 ]; then
		printf 'could not read x11/nvidia-kmod/Makefile (grep status %s)\n' "$_krc" >&2
		return 1
	fi
	return 0
}

expect_export_reject() {
	_name=$1
	_need=$2
	_file=$3
	_rc=0
	check_exports "$_file" 2>"$tmp/why" || _rc=$?
	if [ "$_rc" -eq 0 ]; then
		printf 'FAIL: %s was accepted\n' "$_name" >&2
		exit 1
	fi
	if ! grep -F -q -- "$_need" "$tmp/why"; then
		printf 'FAIL: %s did not report [%s]\n' "$_name" "$_need" >&2
		cat "$tmp/why" >&2
		exit 1
	fi
	note "case $_name: rejected"
}

expect_file_reject() {
	_name=$1
	_need=$2
	_fn=$3
	_file=$4
	_rc=0
	"$_fn" "$_file" 2>"$tmp/why" || _rc=$?
	if [ "$_rc" -eq 0 ]; then
		printf 'FAIL: %s was accepted\n' "$_name" >&2
		exit 1
	fi
	if ! grep -F -q -- "$_need" "$tmp/why"; then
		printf 'FAIL: %s did not report [%s]\n' "$_name" "$_need" >&2
		cat "$tmp/why" >&2
		exit 1
	fi
	note "case $_name: rejected"
}

sed 's/nv_kthread_q_flush/nv_kthread_q_gone/' "$patch_real" > "$tmp/patch-miss"
expect_export_reject "export patch dropped a kthread symbol" \
	"export patch parse missed nv_kthread_q_flush" \
	"$tmp/patch-miss"
cp "$patch_real" "$tmp/patch-extra"
printf '%s\n' '+		rm_gpu_ops_create_session \' >> "$tmp/patch-extra"
expect_export_reject "export patch named an rm_gpu_ops symbol" \
	"stock EXPORT_SYMS already names an rm_gpu_ops_ or nvUvmInterface symbol" \
	"$tmp/patch-extra"
cp "$patch_real" "$tmp/patch-quotes"
printf '%s\n' "+EXPORT_SYMS += rm_gpu_''ops_create_session" >> "$tmp/patch-quotes"
expect_export_reject "export patch hid a name in empty quotes" \
	"stock EXPORT_SYMS already names an rm_gpu_ops_ or nvUvmInterface symbol" \
	"$tmp/patch-quotes"
cp "$patch_real" "$tmp/patch-dollar"
printf '%s\n' '+EXPORT_SYMS += ${HIDDEN}' >> "$tmp/patch-dollar"
expect_export_reject "export patch used a make expansion" \
	"added export line still has a make expansion" \
	"$tmp/patch-dollar"
cp "$patch_real" "$tmp/patch-split"
printf '%s\n' '+rm_gpu_\' '+ops_create_session' >> "$tmp/patch-split"
expect_export_reject "export patch split a name across a continuation" \
	"stock EXPORT_SYMS already names an rm_gpu_ops_ or nvUvmInterface symbol" \
	"$tmp/patch-split"
sed 's/EXPORT_SYMS/EXPORTED/g' "$patch_real" > "$tmp/patch-noassign"
expect_export_reject "export patch dropped the assignment" \
	"added lines do not assign EXPORT_SYMS" \
	"$tmp/patch-noassign"
sed 's/EXPORT_SYMS=/EXPORT_SYMS /' "$patch_real" > "$tmp/patch-mention"
grep -F -q 'EXPORT_SYMS ' "$tmp/patch-mention" \
	|| fail "assignment-mention mutation did not apply"
if grep -F -q 'EXPORT_SYMS=' "$tmp/patch-mention"; then
	fail "assignment-mention mutation did not apply"
fi
expect_export_reject "export patch mentions EXPORT_SYMS without assigning it" \
	"added lines do not assign EXPORT_SYMS" \
	"$tmp/patch-mention"
cp "$patch_real" "$tmp/patch-shell"
printf '%s\n' '+EXPORT_SYMS != printf %s%s rm_gpu_ ops_create_session' >> "$tmp/patch-shell"
grep -F -q '!=' "$tmp/patch-shell" || fail "shell-assignment mutation did not apply"
expect_export_reject "export patch used a shell assignment" \
	"added export line uses a shell assignment" \
	"$tmp/patch-shell"
cp "$patch_real" "$tmp/patch-include"
printf '%s\n' '+.include <syms.mk>' >> "$tmp/patch-include"
grep -F -q '.include <syms.mk>' "$tmp/patch-include" \
	|| fail "include mutation did not apply"
expect_export_reject "export patch includes another makefile" \
	"added export line includes another makefile" \
	"$tmp/patch-include"
cp "$patch_real" "$tmp/patch-context"
printf '%s\n' '+rm_gpu_\' ' ops_create_session' >> "$tmp/patch-context"
grep -F -q 'rm_gpu_\' "$tmp/patch-context" \
	|| fail "context-continuation mutation did not apply"
expect_export_reject "export patch continues onto a context line" \
	"added export line continues onto a non-added line" \
	"$tmp/patch-context"
sed 's/nv_kthread_q_flush/nv_kthread_q_flush_extra/' "$patch_real" > "$tmp/patch-long"
grep -F -q 'nv_kthread_q_flush_extra' "$tmp/patch-long" \
	|| fail "lengthened-name mutation did not apply"
if awk '
	{
		n = split($0, f, /[^A-Za-z0-9_]+/)
		for (i = 1; i <= n; i++) if (f[i] == "nv_kthread_q_flush") found = 1
	}
	END { exit found ? 0 : 1 }
' "$tmp/patch-long"; then
	fail "lengthened-name mutation did not apply"
fi
expect_export_reject "export patch lengthened a kthread name" \
	"export patch parse missed nv_kthread_q_flush" \
	"$tmp/patch-long"
cp "$patch_real" "$tmp/patch-comment-names"
printf '%s\n' '+# nv_kthread_q_flush nv_kthread_q_init nv_kthread_q_init_on_node nv_kthread_q_item_init nv_kthread_q_run_self_test nv_kthread_q_schedule_q_item nv_kthread_q_stop nv_kthread_task_callback' >> "$tmp/patch-comment-names"
grep -F -q '+# nv_kthread_q_flush ' "$tmp/patch-comment-names" \
	|| fail "comment-names mutation did not apply"
expect_export_reject "export patch hid the kthread names in a comment" \
	"added export line is not a literal symbol list" \
	"$tmp/patch-comment-names"

check_upstream_makefile "$tmp/kmod-makefile" \
	|| fail "src/nvidia/Makefile failed the export check"
check_sibling_includes "$kmod_mk" \
	|| fail "an included makefile failed the export check"
cp "$tmp/kmod-makefile" "$tmp/make-split"
printf '%s\n' 'EXPORT_SYM\' 'S= rm_gpu_ops_create_session' >> "$tmp/make-split"
grep -F -q 'EXPORT_SYM\' "$tmp/make-split" \
	|| fail "split EXPORT_SYMS mutation did not apply"
expect_file_reject "upstream makefile split EXPORT_SYMS" \
	"src/nvidia/Makefile sets EXPORT_SYMS" \
	check_upstream_makefile "$tmp/make-split"
cp "$tmp/kmod-makefile" "$tmp/make-angle"
printf '%s\n' '.include <exports.mk>' >> "$tmp/make-angle"
grep -F -q '.include <exports.mk>' "$tmp/make-angle" \
	|| fail "angle-include mutation did not apply"
expect_file_reject "upstream makefile included an unread file" \
	"src/nvidia/Makefile includes a file this check does not read" \
	check_upstream_makefile "$tmp/make-angle"
cp "$tmp/kmod-makefile" "$tmp/make-rel"
printf '%s\n' 'include evil.mk' >> "$tmp/make-rel"
grep -F -q 'include evil.mk' "$tmp/make-rel" \
	|| fail "relative-include mutation did not apply"
expect_file_reject "upstream makefile included a relative file" \
	"src/nvidia/Makefile includes a file this check does not read" \
	check_upstream_makefile "$tmp/make-rel"
mkdir -p "$tmp/empty-nvidia"
printf '%s\n' 'include nvidia-sources.mk' > "$tmp/empty-nvidia/Makefile"
expect_file_reject "included makefile was absent" \
	"nvidia-sources.mk is not beside the Makefile" \
	check_sibling_includes "$tmp/empty-nvidia/Makefile"
printf '%s\n' 'EXPORT_SYMS= rm_gpu_ops_create_session' > "$tmp/nvidia-sources-bad.mk"
grep -F -q 'EXPORT_SYMS=' "$tmp/nvidia-sources-bad.mk" \
	|| fail "sources export mutation did not apply"
expect_file_reject "nvidia-sources.mk set EXPORT_SYMS" \
	"nvidia-sources-bad.mk sets EXPORT_SYMS" \
	scan_included "$tmp/nvidia-sources-bad.mk"
printf '%s\n' '.include <exports.mk>' > "$tmp/optional-cflags-bad.mk"
grep -F -q '.include <exports.mk>' "$tmp/optional-cflags-bad.mk" \
	|| fail "cflags include mutation did not apply"
expect_file_reject "optional-cflags.mk included another makefile" \
	"optional-cflags-bad.mk includes a file this check does not read" \
	scan_included "$tmp/optional-cflags-bad.mk"

kmod_mk_port=$ports_real/x11/nvidia-kmod/Makefile
[ -f "$kmod_mk_port" ] || fail "x11/nvidia-kmod/Makefile is missing"
cp "$kmod_mk_port" "$tmp/kmod-port.mk"
check_kmod_port "$tmp/kmod-port.mk" \
	|| fail "x11/nvidia-kmod/Makefile failed the patch-application check"
cp "$kmod_mk_port" "$tmp/port-override"
printf 'PATCHDIR=\t/tmp/other\n' >> "$tmp/port-override"
grep -F -q "$(printf 'PATCHDIR=\t/tmp/other')" "$tmp/port-override" \
	|| fail "PATCHDIR override mutation did not apply"
expect_file_reject "kmod PATCHDIR overridden" \
	"x11/nvidia-kmod PATCHDIR is not FILESDIR" \
	check_kmod_port "$tmp/port-override"
awk '
	{
		if ($0 ~ /^SUB_PATCHES\+=[[:space:]]+extra-patch-src-Makefile-2$/)
			print "# commented " $0
		else
			print
	}
' "$kmod_mk_port" > "$tmp/port-comment"
grep -F -q '# commented SUB_PATCHES+=' "$tmp/port-comment" \
	|| fail "kmod comment mutation did not apply"
if awk '
	$0 ~ /^SUB_PATCHES\+=[[:space:]]+extra-patch-src-Makefile-2$/ { found = 1 }
	END { exit found ? 0 : 1 }
' "$tmp/port-comment"; then
	fail "kmod comment mutation did not apply"
fi
expect_file_reject "kmod patch name only in a comment" \
	"x11/nvidia-kmod does not apply extra-patch-src-Makefile-2" \
	check_kmod_port "$tmp/port-comment"
awk 'index($0, "${PATCHDIR}/${p}.in") { next } { print }' "$kmod_mk_port" > "$tmp/port-norecipe"
grep -F -q 'FILESDIR=' "$tmp/port-norecipe" \
	|| fail "recipe mutation destroyed the makefile"
if grep -F -q '${PATCHDIR}/${p}.in' "$tmp/port-norecipe"; then
	fail "recipe mutation did not apply"
fi
expect_file_reject "kmod patch recipe removed" \
	"x11/nvidia-kmod does not apply patches from PATCHDIR" \
	check_kmod_port "$tmp/port-norecipe"
cp "$kmod_mk_port" "$tmp/port-shell"
printf '%s\n' 'FOO!= echo injected' >> "$tmp/port-shell"
grep -F -q 'FOO!= echo injected' "$tmp/port-shell" \
	|| fail "kmod shell-assignment mutation did not apply"
expect_file_reject "kmod makefile used a shell assignment" \
	"x11/nvidia-kmod/Makefile uses a shell assignment" \
	check_kmod_port "$tmp/port-shell"

cp "$patch_real" "$tmp/export.patch"
got_patch=$(sha256sum "$tmp/export.patch" | awk '{ print $1 }')
[ "$got_patch" = "$want_patch" ] || fail "export patch hash is $got_patch, expected $want_patch"
check_exports "$tmp/export.patch" \
	|| fail "extra-patch-src-Makefile-2.in is not the eight-name kthread export list"

note "PASS: nv-kernel.o sha256 $want_object has $want_calls global text rm_gpu_ops_ symbols and no defined nvUvmInterface. Those calls are in the NV_UVM_ENABLE arm. src/nvidia/Makefile sha256 $want_makefile sets no EXPORT_SYMS. nvidia-sources.mk sha256 $want_sources and optional-cflags.mk sha256 $want_cflags set no EXPORT_SYMS. extra-patch-src-Makefile-2.in sha256 $want_patch assigns only the eight nv_kthread names. x11/nvidia-kmod applies that patch from nvidia-driver/files. Link plan: compile nv_uvm_interface.c with NV_UVM_ENABLE into nvidia-uvm.ko and add those $want_calls names to that patch."
if ! printf '%s' "$transcript" | cmp -s - "$evidence"; then
	printf 'FAIL: evidence file does not match this run. Write the output above to tests/evidence/symbol-gate.txt.\n' >&2
	printf '%s' "$transcript" >&2
	exit 1
fi
printf '%s' "$transcript"
