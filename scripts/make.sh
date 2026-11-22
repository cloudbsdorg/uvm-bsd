#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# The install/deinstall engine behind the root Makefile.  A root invocation
# copies the two inputs into a private stage before validating anything; a
# normal-user invocation is permitted only with an existing DESTDIR.
#
# Test seams require UVM_TEST_HOOKS=1, a non-empty DESTDIR and effective uid != 0;
# they are never honoured by root.  MK_KO names a planted module and skips the
# build, UVM_TEST_KLDXREF names the kldxref executable, and the hidden
# _check-dest-owner/_check-destdir-ancestors entries expose read-only checks.
# MAKE and MK_SYSDIR are ordinary build configuration, not test seams.

set -eu
umask 022
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin
LC_ALL=C
export PATH LC_ALL

name=uvm-bsd
STATEDIR=/var/db/uvm-bsd
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
repo=$(CDPATH= cd -- "$script_dir/.." && pwd -P)
SYSDIR=${MK_SYSDIR:-/usr/src/sys}
PREFIX=${MK_PREFIX:-/usr/local}
DESTDIR=${MK_DESTDIR:-}
MANIFEST=$DESTDIR$STATEDIR/installed.manifest

die() {
	printf 'make.sh: %s\n' "$*" >&2
	exit 1
}

note() {
	printf 'make.sh: %s\n' "$*"
}

yes_set() {
	[ "${MK_YES:-0}" = 1 ] || [ "${MK_ASSUME_YES:-0}" = 1 ]
}

test_hooks_enabled() {
	[ "${UVM_TEST_HOOKS:-0}" = 1 ] && [ -n "$DESTDIR" ] && [ "$(id -u)" != 0 ]
}

# Without YES=1, a missing terminal is a refusal, never an implicit answer.
ask() {
	if yes_set; then
		return 0
	fi
	if [ ! -t 0 ]; then
		note "$1"
		note "no terminal to ask on: refusing; rerun with YES=1 if you mean it"
		return 1
	fi
	printf '%s [y/N] ' "$1"
	read -r _ans || return 1
	case $_ans in
	y|Y|yes|YES|Yes) return 0 ;;
	esac
	return 1
}

root_method() {
	if [ "$(id -u)" = 0 ]; then
		echo root
	elif command -v doas >/dev/null 2>&1 && [ -r /usr/local/etc/doas.conf ] &&
	    [ "$(doas -C /usr/local/etc/doas.conf sh 2>/dev/null | cut -d ' ' -f 1)" = permit ]; then
		echo doas
	elif command -v sudo >/dev/null 2>&1; then
		echo sudo
	elif [ -t 0 ] && id -Gn | tr ' ' '\n' | grep -qx wheel; then
		echo su
	else
		echo none
	fi
}

as_root() {
	case $(root_method) in
	root) "$@" ;;
	doas) doas "$@" ;;
	sudo) sudo "$@" ;;
	su)
		note "neither doas nor sudo is set up: asking su for the root password"
		su root -c 'exec "$0" "$@"' "$@"
		;;
	*) die "this step needs root: set up doas or sudo for $(id -un), or run make as root" ;;
	esac
}

# dest_ok PATH: an absolute path without ., .., or empty components.  DESTDIR
# is never stored in a manifest record, so spaces and glob characters are safe.
dest_ok() {
	case $1 in
	/*) ;;
	*) return 1 ;;
	esac
	case "$1/" in
	*/./*|*/../*|*//*) return 1 ;;
	esac
	return 0
}

# Manifest records are whitespace-delimited, so their paths reject whitespace.
# Keep this check compatible with paths recorded before PREFIX was narrowed.
path_ok() {
	dest_ok "$1" || return 1
	case $1 in
	*[[:space:]]*) return 1 ;;
	esac
	return 0
}

# Only a command-line PREFIX gets the narrower character check.  A recorded
# PREFIX remains usable by deinstall even when it predates this restriction.
prefix_ok() {
	path_ok "$1" || return 1
	case $1 in
	*[!A-Za-z0-9._/+-]*) return 1 ;;
	esac
	return 0
}

validate_paths() {
	prefix_ok "$PREFIX" || {
		printf 'make.sh: PREFIX must be an absolute path without ., .., whitespace, or empty components; allowed characters are A-Z, a-z, 0-9, ., _, /, +, -\n' >&2
		exit 2
	}
	if [ -n "$DESTDIR" ]; then
		dest_ok "$DESTDIR" || die "DESTDIR must be an absolute path without ., .., or empty components"
		[ -d "$DESTDIR" ] && [ ! -L "$DESTDIR" ] || die "DESTDIR $DESTDIR must be an existing real directory"
	fi
}

# safe_parents ROOT PATH [user]: no existing parent is a symbolic link.  Root
# installs additionally require every existing parent to belong to root.
safe_parents() {
	_sr=$1
	_sp=$(dirname -- "$2")
	_acc=$_sr
	[ -n "$_acc" ] || _acc=/
	_sr_rest=${_sp#/}
	while [ -n "$_sr_rest" ]; do
		case $_sr_rest in
		*/*)
			_sr_comp=${_sr_rest%%/*}
			_sr_rest=${_sr_rest#*/}
			;;
		*)
			_sr_comp=$_sr_rest
			_sr_rest=
			;;
		esac
		[ -n "$_sr_comp" ] || continue
		_acc=${_acc%/}/$_sr_comp
		[ -e "$_acc" ] || [ -L "$_acc" ] || return 0
		[ -d "$_acc" ] && [ ! -L "$_acc" ] || return 1
		[ "${3:-}" = user ] || [ "$(stat -f '%u' "$_acc")" = 0 ] || return 1
	done
	return 0
}

check_dest_owner() {
	[ "$1" = root ] || return 0
	[ -n "$DESTDIR" ] || return 0
	[ "$(stat -f '%u' "$DESTDIR")" = 0 ] || die "refusing DESTDIR $DESTDIR as root: not owned by root"
}

# walk_components BASE ABSOLUTE_PATH CALLBACK: visit BASE and each component
# below it, without inspecting anything above BASE.  Callers choose / for the
# root ancestor check or DESTDIR for directory creation.
walk_components() {
	_wc_base=$1
	_wc_path=$2
	_wc_callback=$3
	case $_wc_base in
	/)
		_wc_acc=/
		_wc_rest=${_wc_path#/}
		;;
	/*)
		case $_wc_path in
		"$_wc_base") _wc_rest= ;;
		"$_wc_base"/*) _wc_rest=${_wc_path#"$_wc_base"/} ;;
		*) die "internal error: path $_wc_path is outside $_wc_base" ;;
		esac
		_wc_acc=$_wc_base
		;;
	*) die "internal error: component walk needs an absolute base" ;;
	esac
	"$_wc_callback" "$_wc_acc"
	while [ -n "$_wc_rest" ]; do
		case $_wc_rest in
		*/*)
			_wc_comp=${_wc_rest%%/*}
			_wc_rest=${_wc_rest#*/}
			;;
		*)
			_wc_comp=$_wc_rest
			_wc_rest=
			;;
		esac
		_wc_acc=${_wc_acc%/}/$_wc_comp
		"$_wc_callback" "$_wc_acc"
	done
}

check_dest_component() {
	_ca_path=$1
	[ -e "$_ca_path" ] || [ -L "$_ca_path" ] || return 0
	[ -d "$_ca_path" ] && [ ! -L "$_ca_path" ] ||
	    die "refusing DESTDIR component $_ca_path: not a real directory"
	_ca_uid=$(stat -f '%u' "$_ca_path") ||
	    die "refusing DESTDIR component $_ca_path: cannot read owner"
	if [ "$_ca_uid" != 0 ] &&
	    { [ -z "$_ca_test_uid" ] || [ "$_ca_uid" != "$_ca_test_uid" ]; }; then
		die "refusing DESTDIR component $_ca_path as root: not owned by root"
	fi
	# %p retains special-mode bits; FreeBSD's %Lp omits the sticky bit.
	_ca_mode=$(stat -f '%p' "$_ca_path") ||
	    die "refusing DESTDIR component $_ca_path: cannot read mode"
	# The ownership check above makes sticky + trusted owner the sole exception.
	if [ "$((0$_ca_mode & 0022))" -ne 0 ] && [ "$((0$_ca_mode & 01000))" -eq 0 ]; then
		die "refusing DESTDIR component $_ca_path as root: writable by group or other without sticky-bit protection"
	fi
}

# check_dest_ancestors user|root DESTDIR [TEST_UID]: only root mode checks the
# ancestors above DESTDIR.  TEST_UID is supplied only by the read-only hidden
# test entry, whose own temporary directories stand in for root-owned ones.
check_dest_ancestors() {
	[ "$1" = root ] || return 0
	[ -n "$2" ] || return 0
	_ca_test_uid=${3:-}
	walk_components / "$2" check_dest_component
}

stage_files() {
	(cd "$1" && find . \( -type f -o -type l \) | sed 's|^\.||' | sort)
}

allowed_path() {
	[ "$1" = /boot/modules/nvidia-uvm.ko ] || [ "$1" = "$2/share/man/man4/nvidia-uvm.4" ]
}

manifest_prefix() {
	awk 'NR == 1 && $1 == "prefix" && NF == 2 { print $2; found = 1 } END { exit !found }' "$1"
}

mkdir_component() {
	_mp_path=$1
	if [ -L "$_mp_path" ] || { [ -e "$_mp_path" ] && [ ! -d "$_mp_path" ]; }; then
		die "cannot create directory $_mp_path: not a real directory"
	fi
	[ -d "$_mp_path" ] || mkdir -m 755 -- "$_mp_path" || die "cannot create directory $_mp_path"
}

# mkdir_p755 ABSOLUTE_PATH: create only missing components at or below DESTDIR
# (or below / for a live install), with an explicit mode.  Existing directories
# are left alone.  Private mktemp directories stay 700.
mkdir_p755() {
	case $1 in
	/*) ;;
	*) die "internal error: mkdir_p755 needs an absolute path" ;;
	esac
	walk_components "${DESTDIR:-/}" "$1" mkdir_component
}

# The private input copy prevents a user-owned source from changing between
# validation and the privileged copy.  A source swapped to a link is refused.
copy_source() {
	_cs=$1
	_cdest=$2
	_cmode=$3
	_cwork=$4
	[ -f "$_cs" ] && [ ! -L "$_cs" ] || die "input $_cs is not a regular file"
	_cdir=$(dirname -- "$_cs")
	_cbase=$(basename -- "$_cs")
	case $_cbase in
	''|.|..|*[!A-Za-z0-9_.-]*) die "input $_cs has an unsafe basename" ;;
	esac
	mkdir -m 755 -- "$_cwork"
	_carchive=$_cwork.tar
	tar -C "$_cdir" -cf "$_carchive" "./$_cbase" || die "cannot read private source $_cs"
	tar -C "$_cwork" -xpf "$_carchive" || die "cannot extract private source $_cs"
	rm -f -- "$_carchive"
	[ -f "$_cwork/$_cbase" ] && [ ! -L "$_cwork/$_cbase" ] || die "input $_cs changed or is not a regular file"
	install -m "$_cmode" -- "$_cwork/$_cbase" "$_cdest"
}

manifest_line() {
	printf '%s %s %s %s\n' "$2" "$(stat -f '%Lp' "$1$2")" "$(stat -f '%Su:%Sg' "$1$2")" "$(sha256 -q "$1$2")"
}

pending_line() {
	printf '%s pending %s\n' "$2" "$(sha256 -q "$1$2")"
}

line_matches() {
	_r=$1
	set -- $2
	[ -e "$_r$1" ] || return 1
	case $2 in
	dir) return 1 ;;
	pending) [ -f "$_r$1" ] && [ ! -L "$_r$1" ] && [ "$(sha256 -q "$_r$1")" = "$3" ] ;;
	*) [ -f "$_r$1" ] && [ ! -L "$_r$1" ] && [ "$4" != - ] && [ "$(sha256 -q "$_r$1")" = "$4" ] ;;
	esac
}

# A path may be replaced or removed when any current or pending record for it
# matches the bytes on disk.  Pending records make an interrupted copy safe to
# rerun or deinstall.
still_ours() {
	_so=1
	_lines=$(awk -v p="$3" '$1 == p && $2 != "dir"' "$2")
	[ -n "$_lines" ] || return 1
	while read -r _ln; do
		if line_matches "$1" "$_ln"; then
			_so=0
			break
		fi
	done <<EOF
$_lines
EOF
	return "$_so"
}

# kldxref is a post-install refresh, not a reason to undo completed files.
# UVM_TEST_KLDXREF replaces only the command when the test hooks are enabled.
run_kldxref() {
	_kx=kldxref
	if test_hooks_enabled && [ -n "${UVM_TEST_KLDXREF:-}" ]; then
		_kx=$UVM_TEST_KLDXREF
	fi
	if "$_kx" "$1"; then
		return 0
	fi
	if [ -z "$DESTDIR" ]; then
		note "kldxref $1 failed: the module is installed but the loader index is stale; run doas kldxref $1"
		return 1
	fi
	note "kldxref $1 failed"
	return 0
}

# install_stage DESTDIR PREFIX user|root KO MAN: validate all destinations
# before copying either payload, write pending records, then replace atomically.
install_stage() {
	_d=$1
	_pfx=$2
	_mode=$3
	_ko_source=$4
	_man_source=$5
	_manifest=$_d$STATEDIR/installed.manifest
	[ "$_mode" != root ] || [ "$(id -u)" = 0 ] || die "the root install step was not entered through as_root"
	check_dest_owner "$_mode"
	check_dest_ancestors "$_mode" "$_d"
	safe_parents "$_d" "$STATEDIR/x" "$_mode" || die "refusing $_d$STATEDIR: a directory on the way is a symbolic link or not root's"
	if [ -e "$_manifest" ] || [ -L "$_manifest" ]; then
		[ -f "$_manifest" ] && [ ! -L "$_manifest" ] || die "refusing $_manifest: not a regular manifest"
		_recorded_pfx=$(manifest_prefix "$_manifest") || die "refusing $_manifest: first line is not a prefix record"
		path_ok "$_recorded_pfx" || die "refusing $_manifest: invalid recorded PREFIX"
		[ "$_recorded_pfx" = "$_pfx" ] || die "refusing PREFIX $_pfx: manifest records $_recorded_pfx; deinstall first"
	fi

	_newdirs=
	_dir=$STATEDIR
	while [ "$_dir" != / ] && [ ! -d "$_d$_dir" ]; do
		_newdirs="$_dir
$_newdirs"
		_dir=$(dirname -- "$_dir")
	done
	_w=
	trap '[ -z "$_w" ] || rm -rf -- "$_w"' EXIT
	trap 'exit 129' HUP
	trap 'exit 130' INT
	trap 'exit 143' TERM
	_w=$(mktemp -d "$_d/.uvm-bsd.install.XXXXXX") || die "cannot create a private directory in $_d"
	_s=$_w/stage
	mkdir_p755 "$_s/boot/modules"
	mkdir_p755 "$_s$_pfx/share/man/man4"
	copy_source "$_ko_source" "$_s/boot/modules/nvidia-uvm.ko" 555 "$_w/input-ko"
	copy_source "$_man_source" "$_s$_pfx/share/man/man4/nvidia-uvm.4" 444 "$_w/input-man"
	stage_files "$_s" > "$_w/list"
	[ "$(wc -l < "$_w/list" | tr -d ' ')" = 2 ] || die "the private stage does not contain exactly two files"

	_prev=$_w/prev
	: > "$_prev"
	if [ -f "$_manifest" ]; then
		cp -- "$_manifest" "$_prev"
	fi
	while read -r _f; do
		path_ok "$_f" && allowed_path "$_f" "$_pfx" || die "refusing staged path $_f: not a file make install writes"
		[ ! -L "$_s$_f" ] || die "refusing staged symbolic link $_f"
		safe_parents "$_d" "$_f" "$_mode" || die "refusing $_d$_f: a directory on the way is a symbolic link or not root's"
		if [ -e "$_d$_f" ] || [ -L "$_d$_f" ]; then
			awk -v p="$_f" '$1 == p { found = 1 } END { exit !found }' "$_prev" ||
			    die "refusing to replace $_d$_f: make install did not write it"
			still_ours "$_d" "$_prev" "$_f" || die "refusing to replace $_d$_f: it changed since make install wrote it"
		fi
	done < "$_w/list"

	# No refusal above has created STATEDIR or any of its parents.
	mkdir_p755 "$_d$STATEDIR"
	while read -r _f; do
		_dir=$(dirname -- "$_f")
		while [ "$_dir" != / ] && [ ! -d "$_d$_dir" ]; do
			_newdirs="$_newdirs
$_dir"
			_dir=$(dirname -- "$_dir")
		done
		mkdir_p755 "$(dirname -- "$_d$_f")"
	done < "$_w/list"
	{
		printf 'prefix %s\n' "$_pfx"
		awk 'NR > 1' "$_prev"
		while read -r _f; do
			pending_line "$_s" "$_f"
		done < "$_w/list"
		printf '%s\n' "$_newdirs" | awk 'NF { print $0 " dir" }'
	} | awk '!seen[$0]++' > "$_manifest.new"
	mv -f -- "$_manifest.new" "$_manifest"

	while read -r _f; do
		if [ "$_mode" = root ]; then
			install -o root -g wheel -m "$(stat -f '%Lp' "$_s$_f")" -- "$_s$_f" "$_d$_f.new"
		else
			install -m "$(stat -f '%Lp' "$_s$_f")" -- "$_s$_f" "$_d$_f.new"
		fi
		mv -f -- "$_d$_f.new" "$_d$_f"
	done < "$_w/list"

	{
		printf 'prefix %s\n' "$_pfx"
		while read -r _f; do
			manifest_line "$_d" "$_f"
		done < "$_w/list"
		printf '%s 444 %s -\n' "$STATEDIR/installed.manifest" "$(stat -f '%Su:%Sg' "$_d$STATEDIR")"
		awk '$2 == "dir"' "$_prev"
		printf '%s\n' "$_newdirs" | awk 'NF { print $0 " dir" }'
	} | awk '!seen[$0]++' > "$_manifest.new"
	mv -f -- "$_manifest.new" "$_manifest"
	chmod 444 "$_manifest"
	rm -rf -- "$_w"
	_w=
	trap - EXIT HUP INT TERM

	run_kldxref "$_d/boot/modules"
}

do_install() {
	_ko=$repo/sys/nvidia-uvm.ko
	if test_hooks_enabled && [ -n "${MK_KO:-}" ]; then
		_ko=$MK_KO
	else
		(cd "$repo/sys" && ${MAKE:-make} SYSDIR="$SYSDIR") || die "install: building nvidia-uvm.ko failed"
	fi
	[ -f "$_ko" ] && [ ! -L "$_ko" ] || die "install: $_ko is not a regular file"
	_man=$repo/man/nvidia-uvm.4
	[ -f "$_man" ] && [ ! -L "$_man" ] || die "install: $_man is not a regular file"
	if [ -n "$DESTDIR" ] && [ "$(id -u)" != 0 ]; then
		ask "Install nvidia-uvm.ko and its man page into $DESTDIR (owned by $(id -un))?" || die "install: nothing installed"
		validate_paths
		install_stage "$DESTDIR" "$PREFIX" user "$_ko" "$_man"
	else
		ask "Install nvidia-uvm.ko and its man page as root (owner root:wheel)?" || die "install: nothing installed"
		validate_paths
		as_root sh "$repo/scripts/make.sh" _install-stage "$DESTDIR" "$PREFIX" root "$_ko" "$_man"
	fi
	note "install: done; manifest $MANIFEST"
}

keep_path() {
	awk -v p="$3" '$1 == p' "$2" >> "$1"
}

# Changed or unsafe paths stay listed.  Everything else install wrote is
# removed, followed by its recorded directories, deepest first.
deinstall_manifest() {
	_d=$1
	_mode=$3
	_manifest=$_d$STATEDIR/installed.manifest
	check_dest_owner "$_mode"
	check_dest_ancestors "$_mode" "$_d"
	[ -f "$_manifest" ] || return 0
	[ ! -L "$_manifest" ] || die "refusing $_manifest: symbolic link"
	safe_parents "$_d" "$STATEDIR/installed.manifest" "$_mode" || die "refusing $_manifest: a directory on the way is a symbolic link or not root's"
	_recorded_pfx=$(manifest_prefix "$_manifest") || die "refusing $_manifest: first line is not a prefix record"
	path_ok "$_recorded_pfx" || die "refusing $_manifest: invalid recorded PREFIX"
	_kept=$_manifest.kept
	[ ! -L "$_kept" ] || die "refusing $_kept: symbolic link"
	: > "$_kept"

	awk '$1 != "prefix" && $2 != "dir" { print $1 }' "$_manifest" | sort -u | while read -r _f; do
		[ "$_f" = "$STATEDIR/installed.manifest" ] && continue
		if ! path_ok "$_f" || ! allowed_path "$_f" "$_recorded_pfx"; then
			note "deinstall: refusing manifest path $_f"
			keep_path "$_kept" "$_manifest" "$_f"
			continue
		fi
		if ! safe_parents "$_d" "$_f" "$_mode"; then
			note "deinstall: refusing $_d$_f: a directory on the way is a symbolic link or not root's"
			keep_path "$_kept" "$_manifest" "$_f"
			continue
		fi
		if awk -v p="$_f" '$1 == p && $2 == "pending" { found = 1 } END { exit !found }' "$_manifest"; then
			rm -f -- "$_d$_f.new"
		fi
		if [ ! -e "$_d$_f" ] && [ ! -L "$_d$_f" ]; then
			continue
		fi
		if still_ours "$_d" "$_manifest" "$_f"; then
			rm -f -- "$_d$_f"
		else
			note "deinstall: kept $_d$_f (changed since make install wrote it)"
			keep_path "$_kept" "$_manifest" "$_f"
		fi
	done

	_dirs=$(awk '$2 == "dir" { print $1 }' "$_manifest" | sort -r)
	_modules_created=0
	if awk '$1 == "/boot/modules" && $2 == "dir" { found = 1 } END { exit !found }' "$_manifest"; then
		_modules_created=1
	fi
	_rc=0
	if [ -s "$_kept" ]; then
		awk '$2 == "dir"' "$_manifest" >> "$_kept"
		{
			printf 'prefix %s\n' "$_recorded_pfx"
			cat "$_kept"
		} > "$_manifest.new"
		mv -f -- "$_manifest.new" "$_manifest"
		rm -f -- "$_kept"
		chmod 444 "$_manifest"
		_rc=1
	else
		rm -f -- "$_kept" "$_manifest"
	fi
	# Refresh linker.hints while its directory still exists.  Install owns the
	# hints file only when it created boot/modules; even then, leave refreshed
	# hints behind if another file now needs that directory.
	if [ -d "$_d/boot/modules" ] && [ ! -L "$_d/boot/modules" ] &&
	    safe_parents "$_d" /boot/modules/linker.hints "$_mode"; then
		run_kldxref "$_d/boot/modules"
		if [ "$_modules_created" = 1 ] &&
		    [ -z "$(find "$_d/boot/modules" -mindepth 1 -maxdepth 1 ! -name linker.hints -print -quit)" ]; then
			rm -f -- "$_d/boot/modules/linker.hints"
		fi
	fi
	printf '%s\n' "$_dirs" | while read -r _dir; do
		[ -n "$_dir" ] || continue
		path_ok "$_dir" && safe_parents "$_d" "$_dir/x" "$_mode" || continue
		if [ -e "$_d$_dir" ] || [ -L "$_d$_dir" ]; then
			rmdir -- "$_d$_dir" 2>/dev/null || note "deinstall: kept $_d$_dir (not empty)"
		fi
	done
	[ "$_rc" = 0 ] && note "deinstall: done" || note "deinstall: done, with changed files kept (listed in $_manifest)"
	return "$_rc"
}

do_deinstall() {
	validate_paths
	if [ ! -f "$MANIFEST" ]; then
		note "deinstall: nothing installed (no $MANIFEST)"
		return 0
	fi
	if [ -z "$DESTDIR" ] && kldstat -q -n nvidia-uvm 2>/dev/null; then
		die "deinstall: nvidia-uvm is loaded; unload it first (or reboot)"
	fi
	if [ -n "$DESTDIR" ] && [ "$(id -u)" != 0 ]; then
		ask "Remove the files listed in $MANIFEST?" || die "deinstall: nothing removed"
		deinstall_manifest "$DESTDIR" "$PREFIX" user
	else
		ask "Remove the files listed in $MANIFEST as root?" || die "deinstall: nothing removed"
		as_root sh "$repo/scripts/make.sh" _deinstall "$DESTDIR" "$PREFIX" root
	fi
}

case ${1:-} in
_check-dest-owner)
	[ "$#" = 3 ] || die "usage: UVM_TEST_HOOKS=1 make.sh _check-dest-owner user|root DESTDIR"
	case $2 in
	user|root) ;;
	*) die "usage: UVM_TEST_HOOKS=1 make.sh _check-dest-owner user|root DESTDIR" ;;
	esac
	DESTDIR=$3
	test_hooks_enabled || die "test hooks are disabled"
	validate_paths
	check_dest_owner "$2"
	;;
_check-destdir-ancestors)
	[ "$#" = 3 ] || die "usage: UVM_TEST_HOOKS=1 make.sh _check-destdir-ancestors user|root DESTDIR"
	case $2 in
	user|root) ;;
	*) die "usage: UVM_TEST_HOOKS=1 make.sh _check-destdir-ancestors user|root DESTDIR" ;;
	esac
	DESTDIR=$3
	test_hooks_enabled || die "test hooks are disabled"
	validate_paths
	check_dest_ancestors "$2" "$DESTDIR" "$(id -u)"
	;;
install-prereqs)
	_missing=
	for _tool in cc make mandoc; do
		if ! command -v "$_tool" >/dev/null 2>&1; then
			_missing="$_missing $_tool"
		fi
	done
	if [ -n "$_missing" ]; then
		note "install-prereqs: missing:${_missing}"
		exit 1
	fi
	note "install-prereqs: nothing to install"
	;;
configure)
	note "nothing to configure"
	;;
install)
	do_install
	;;
deinstall)
	do_deinstall
	;;
_install-stage)
	[ "$#" = 6 ] || die "usage: make.sh _install-stage DESTDIR PREFIX user|root KO MAN"
	case $4 in
	user|root) ;;
	*) printf 'make.sh: invalid install mode: %s\n' "$4" >&2; exit 2 ;;
	esac
	DESTDIR=$2
	PREFIX=$3
	MANIFEST=$DESTDIR$STATEDIR/installed.manifest
	validate_paths
	install_stage "$DESTDIR" "$PREFIX" "$4" "$5" "$6"
	;;
_deinstall)
	[ "$#" = 4 ] || die "usage: make.sh _deinstall DESTDIR PREFIX user|root"
	case $4 in
	user|root) ;;
	*) printf 'make.sh: invalid deinstall mode: %s\n' "$4" >&2; exit 2 ;;
	esac
	DESTDIR=$2
	PREFIX=$3
	MANIFEST=$DESTDIR$STATEDIR/installed.manifest
	validate_paths
	deinstall_manifest "$DESTDIR" "$PREFIX" "$4"
	;;
*)
	die "usage: make.sh install-prereqs|configure|install|deinstall"
	;;
esac
