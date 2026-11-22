#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# make-be.sh - assemble the one-shot test boot environment for the RM client ownership test boot (Track #2373).
#
# Usage: make-be.sh [-N NORMAL_BE] [-b BASE_BE] [-D YYYYMMDD] [-n SEQ] [-m MODDIR] [-s SHA256FILE]
#                   [--dry-run] [--activate]      (run as root)
#   -N NORMAL_BE   the boot environment the host must return to (default cloudbsd-rx7600); testboot-return
#                  brings the host back to it after the run
#   -b BASE_BE     the boot environment to clone (default NORMAL_BE)
#   -D DATE, -n SEQ   the name is nvidia-client-owner-test-<DATE>-<SEQ> (today, 1)
#   -m MODDIR      the directory holding the staged v5 modules and the hash list (default
#                  $GB10_WORK/logs/uvm-2373)
#   -s FILE        the hash list (default MODDIR/SHA256-v5.txt)
#   --dry-run      everything except `bectl activate`: the boot environment is built and checked, never armed
#                  (this is the default; the option says so)
#   --activate     finish with `bectl activate -t BE`: arms the one-shot boot.  Arming is the orchestrator's
#                  decision: do not use it while a Fable agent runs on the host
#
# What it does, in order (any failure after the clone exists removes the half-built boot environment):
#   1. reads the hash list: the four staged v5 modules (nvidia-v5-phase0.ko, nvidia-modeset-v5.ko,
#      nvidia-drm-v5.ko, nvidia-uvm-v5.ko) each need a full literal 64-digit sha256 entry and each staged file
#      must equal its entry (nvidia-drm is checked and NOT installed: the kit does not load it); the vendored
#      testboot-return must equal the pin in VENDOR; the kit must be committed (git status clean in sys/ and
#      tests/ioctl), and what is installed is `git archive` of that commit, never the work tree;
#   2. bectl create -e BASE BE, mounts it on a new root-owned directory of /var/run, and refuses a clone that
#      would load an nvidia module by itself (kld_list, loader.conf, loader.conf.d, rc.conf.d) or has one in /boot/modules
#      or /boot/kernel;
#   3. copies nvidia.ko, nvidia-modeset.ko and nvidia-uvm.ko, as root, into the NEW root-owned directory
#      /boot/modules.nvco-test of the clone (refused when it exists), mode 0444, and checks every copy against
#      the same full hash again;
#   4. installs the kit (this repository's sys/ and tests/ioctl/, root-owned, all from one git archive of the commit,
#      the rc script, summarize.sh and testboot-return included), the hash list, the CUDA script
#      (root-owned), the rc one-shot nvidia_client_owner_testboot and its rc.conf.d file, testboot-return
#      (vendored: see VENDOR), turns off every other test-boot kit in the clone, and writes loader.conf lines
#      kern.panic_reboot_wait_time=5 and debug.debugger_on_panic=0 into the clone only;
#   5. unmounts; with --activate, arms the one-shot boot and says what to read after it.
# The running system is not touched: nothing is loaded, unloaded or rebooted.
#
# Environment: GB10_WORK (default /home/mlapointe/work/gb10).  Test hooks, honoured only with GB10_TEST_HOOKS=1
# AND when not root (bectl then cannot arm anything real): NVCO_BECTL (the bectl to run), NVCO_MNT (the clone's mount point),
# NVCO_NOT_ROOT (install without root ownership), NVCO_NO_GIT_CHECK (no git check; the work tree is installed).

set -eu
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
LC_ALL=C
export PATH LC_ALL

KITDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$KITDIR/../../.." && pwd)
WORK=${GB10_WORK:-/home/mlapointe/work/gb10}
BECTL=bectl
HOOKS=0
if [ "${GB10_TEST_HOOKS:-0}" = 1 ] && [ "$(id -u)" != 0 ]; then HOOKS=1; BECTL=${NVCO_BECTL:-bectl}; fi
NORMAL=cloudbsd-rx7600
BASE=
DATE=$(date -u +%Y%m%d)
SEQ=1
MODDIR=
SHAFILE=
ACTIVATE=0

usage() { sed -n '5,/^# Environment/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
need() { [ "$2" -ge 2 ] || { echo "make-be.sh: $1 needs a value" >&2; usage; }; }
while [ $# -gt 0 ]; do
	case $1 in
	-N) need "$1" "$#"; NORMAL=$2; shift 2 ;;
	-b) need "$1" "$#"; BASE=$2; shift 2 ;;
	-D) need "$1" "$#"; DATE=$2; shift 2 ;;
	-n) need "$1" "$#"; SEQ=$2; shift 2 ;;
	-m) need "$1" "$#"; MODDIR=$2; shift 2 ;;
	-s) need "$1" "$#"; SHAFILE=$2; shift 2 ;;
	--dry-run) ACTIVATE=0; shift ;;
	--activate) ACTIVATE=1; shift ;;
	-h | --help) usage ;;
	*) echo "make-be.sh: unknown argument: $1" >&2; usage ;;
	esac
done
[ -n "$BASE" ] || BASE=$NORMAL
[ -n "$MODDIR" ] || MODDIR=$WORK/logs/uvm-2373
[ -n "$SHAFILE" ] || SHAFILE=$MODDIR/SHA256-v5.txt
case $DATE in [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]) ;; *) echo "make-be.sh: -D must be YYYYMMDD" >&2; exit 2 ;; esac
case $SEQ in '' | *[!0-9]*) echo "make-be.sh: -n must be a number" >&2; exit 2 ;; esac
for _n in "$NORMAL" "$BASE"; do
	case $_n in *[!A-Za-z0-9._-]* | -* | '') echo "make-be.sh: unusable boot environment name: $_n" >&2; exit 2 ;; esac
done
if [ "$HOOKS" = 0 ] && [ "$(id -u)" != 0 ]; then echo "make-be.sh: run as root (bectl and the root-owned installs need it)" >&2; exit 2; fi
if [ "$HOOKS" = 1 ] && [ -n "${NVCO_NOT_ROOT:-}" ]; then ROOTOWN=; else ROOTOWN="-o root -g wheel"; fi

BE=nvidia-client-owner-test-$DATE-$SEQ
[ "$BASE" != "$BE" ] || { echo "make-be.sh: the base boot environment is the target name" >&2; exit 2; }
[ "$NORMAL" != "$BE" ] || { echo "make-be.sh: the normal boot environment is the target name" >&2; exit 2; }

# the mount point: a new directory of a root-owned parent (never a path another user can name beforehand)
if [ "$HOOKS" = 1 ] && [ -n "${NVCO_MNT:-}" ]; then
	MNT=$NVCO_MNT
else
	MNTBASE=$(mktemp -d /var/run/nvco.XXXXXXXX)
	MNT=$MNTBASE/root
fi
SCRATCH=$(dirname "$MNT")
mkdir -p "$SCRATCH"
early_cleanup() { rm -rf "$SCRATCH/kit" "$SCRATCH/kit.tar"; [ -z "${MNTBASE:-}" ] || rmdir "$MNTBASE" 2> /dev/null; }
trap early_cleanup EXIT

# the kit itself: committed, so that what boots is what was reviewed.  git is told the directory is safe and not to
# write the index (root runs on the owner's repository)
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_COMMON_DIR
git_() { git -c safe.directory="$REPO" --no-optional-locks -C "$REPO" "$@"; }
if [ "$HOOKS" = 1 ] && [ -n "${NVCO_NO_GIT_CHECK:-}" ]; then
	COMMIT=unchecked-test-hook
else
	git_ rev-parse --git-dir > /dev/null 2>&1 || { echo "make-be.sh: $REPO is not a git checkout, or git refuses it (safe.directory); run from the git clone of the kit" >&2; exit 2; }
	_dirty=$(git_ status --porcelain -- sys tests/ioctl 2>/dev/null || echo unreadable)
	[ -z "$_dirty" ] || { echo "make-be.sh: the kit has uncommitted changes in sys/ or tests/ioctl/ (commit them; the commit is what gets installed and recorded):
$_dirty" >&2; exit 2; }
	COMMIT=$(git_ rev-parse HEAD)
fi

# 1. the staged modules against the full literal hashes of the list
literal() {	# literal <name>: the single full 64-digit hash the list gives the file, or nothing
	awk -v n="($1)" '$1 == "SHA256" && $2 == n && $3 == "=" && length($4) == 64 && $4 ~ /^[0-9a-f]+$/ { print $4; c++ } END { exit (c == 1 ? 0 : 1) }' "$SHAFILE"
}
[ -f "$SHAFILE" ] || { echo "make-be.sh: no hash list $SHAFILE" >&2; exit 1; }
[ "$(grep -c '^SHA256 (' "$SHAFILE")" = 4 ] || { echo "make-be.sh: $SHAFILE must hold exactly four SHA256 entries" >&2; exit 1; }
INSTALL="nvidia-v5-phase0.ko:nvidia.ko nvidia-modeset-v5.ko:nvidia-modeset.ko nvidia-uvm-v5.ko:nvidia-uvm.ko"
for _p in $INSTALL nvidia-drm-v5.ko:; do
	_f=${_p%%:*}
	_want=$(literal "$_f") || { echo "make-be.sh: $SHAFILE has no single full 64-digit sha256 entry for $_f" >&2; exit 1; }
	[ -f "$MODDIR/$_f" ] || { echo "make-be.sh: missing $MODDIR/$_f" >&2; exit 1; }
	_got=$(sha256 -q "$MODDIR/$_f")
	[ "$_got" = "$_want" ] || { echo "make-be.sh: $MODDIR/$_f is $_got, the list says $_want" >&2; exit 1; }
	echo "checked $_f $_got"
done
CUDASRC=$WORK/nvidia-gb10-freebsd-uvm0/scripts/run-linux-cuda.sh
[ -f "$CUDASRC" ] || { echo "make-be.sh: missing the CUDA vecadd script $CUDASRC" >&2; exit 1; }
# what is installed is the commit, never the work tree (ignored files, a later edit): the sources, the rc script,
# summarize.sh and testboot-return all come from this one archive
if [ "$COMMIT" = unchecked-test-hook ]; then
	( cd "$REPO" && tar -cf "$SCRATCH/kit.tar" --exclude sys/machine sys tests/ioctl )
else
	git_ archive "$COMMIT" sys tests/ioctl > "$SCRATCH/kit.tar" || { echo "make-be.sh: git archive of $COMMIT failed" >&2; exit 1; }
fi
rm -rf "$SCRATCH/kit"
mkdir "$SCRATCH/kit" || { echo "make-be.sh: cannot make $SCRATCH/kit" >&2; exit 1; }
tar -xf "$SCRATCH/kit.tar" -C "$SCRATCH/kit" || { echo "make-be.sh: the archive of commit $COMMIT does not unpack (disk full?)" >&2; exit 1; }
# nothing but regular files and directories: a committed symbolic link would be followed at install
[ -z "$(find "$SCRATCH/kit" ! -type f ! -type d)" ] || { echo "make-be.sh: commit $COMMIT holds a symbolic link or special file under sys/ or tests/ioctl/: the kit installs regular files only" >&2; exit 1; }
ASRC=$SCRATCH/kit/tests/ioctl/testboot
# the vendored testboot-return of the commit is the one that was pinned
VWANT=$(sed -n 's/^sha256 *= *\([0-9a-f]\{64\}\)$/\1/p' "$ASRC/VENDOR" 2> /dev/null)
[ -n "$VWANT" ] && [ -f "$ASRC/testboot-return" ] && [ "$(sha256 -q "$ASRC/testboot-return")" = "$VWANT" ] || { echo "make-be.sh: testboot-return in commit $COMMIT differs from the pin in VENDOR (or VENDOR is missing)" >&2; exit 1; }
for _f in nvidia_client_owner_testboot summarize.sh; do [ -f "$ASRC/$_f" ] || { echo "make-be.sh: commit $COMMIT has no tests/ioctl/testboot/$_f (not committed, or ignored by .gitignore?): commit it and run again" >&2; exit 1; }; done

run() { echo "+ $*"; "$@"; }
BE_MADE=0
BE_DONE=0
cleanup() {
	_rc=$?
	trap '' INT TERM HUP
	if [ "$BE_MADE" -eq 1 ] && [ "$BE_DONE" -eq 0 ]; then
		echo "make-be.sh: failed (exit $_rc); removing the half-built boot environment $BE" >&2
		$BECTL umount -f "$BE" > /dev/null 2>&1 || true
		if $BECTL destroy -F "$BE" > /dev/null 2>&1; then
			echo "make-be.sh: $BE is removed (the snapshot it was cloned from stays on $BASE; see bectl list -s)" >&2
		else
			echo "make-be.sh: COULD NOT remove $BE: remove it by hand with bectl(8) before using the name again" >&2
		fi
	fi
	rm -rf "$SCRATCH/kit" "$SCRATCH/kit.tar"
	[ -z "${MNTBASE:-}" ] || { rmdir "$MNT" 2> /dev/null; rmdir "$MNTBASE" 2> /dev/null; }
	true
	exit "$_rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# 2. clone
run $BECTL create -e "$BASE" "$BE"
BE_MADE=1
run mkdir -p "$MNT"
[ ! -L "$MNT" ] || { echo "make-be.sh: the mount point $MNT is a symbolic link" >&2; exit 1; }
run $BECTL mount "$BE" "$MNT"

# the clone must not load an nvidia module by itself: the kit loads them, from its own directory, and nothing else.  A
# line continued with a backslash is joined first.
loads_nvidia() {	# loads_nvidia <file>
	sed -e :a -e '/\\$/N; s/\\\n//; ta' "$1" 2> /dev/null | grep -Eq '^[[:space:]]*(kld_list=.*nvidia|nvidia[A-Za-z0-9_-]*_load=.?[Yy])'
}
for _f in etc/rc.conf etc/rc.conf.local usr/local/etc/rc.conf usr/local/etc/rc.conf.local boot/loader.conf boot/loader.conf.local; do
	[ -f "$MNT/$_f" ] || continue
	if loads_nvidia "$MNT/$_f"; then
		echo "make-be.sh: $_f of $BASE loads an nvidia module by itself: this kit loads them, from /boot/modules.nvco-test; use a base boot environment without them" >&2
		exit 1
	fi
done
for _d in etc/rc.conf.d usr/local/etc/rc.conf.d boot/loader.conf.d; do
	[ -d "$MNT/$_d" ] || continue
	_hit=$(find "$MNT/$_d" -type f | while IFS= read -r _g; do if loads_nvidia "$_g"; then echo "$_g"; fi; done)
	[ -z "$_hit" ] || { echo "make-be.sh: $_hit of $BASE loads an nvidia module by itself" >&2; exit 1; }
done
for _d in boot/modules boot/kernel; do
	if ls "$MNT/$_d"/nvidia*.ko > /dev/null 2>&1; then
		echo "make-be.sh: $_d of $BASE holds an nvidia module: a load by name could pick it instead of the staged one" >&2; exit 1
	fi
done

# 3. modules: a NEW root-owned directory
MODS=$MNT/boot/modules.nvco-test
[ ! -e "$MODS" ] || { echo "make-be.sh: $MODS already exists in the clone" >&2; exit 1; }
run install -d $ROOTOWN -m 0755 "$MODS"
for _p in $INSTALL; do
	_f=${_p%%:*}; _d=${_p#*:}
	_want=$(literal "$_f")
	run install $ROOTOWN -m 0444 "$MODDIR/$_f" "$MODS/$_d"
	_got=$(sha256 -q "$MODS/$_d")
	[ "$_got" = "$_want" ] || { echo "make-be.sh: $_d differs after the copy ($_got, the list says $_want); the half-built boot environment is removed" >&2; exit 1; }
	echo "installed $_d $_got"
done
[ ! -e "$MODS/nvidia-drm.ko" ] || { echo "make-be.sh: nvidia-drm.ko is in the module directory" >&2; exit 1; }

# 4. the kit
KROOT=$MNT/usr/local/share/nvidia-client-owner-test
run install -d $ROOTOWN -m 0755 "$KROOT" "$KROOT/repo" "$MNT/usr/local/sbin" "$MNT/usr/local/etc/rc.d" "$MNT/etc/rc.conf.d" "$MNT/var/db"
tar -xf "$SCRATCH/kit.tar" -C "$KROOT/repo" || { echo "make-be.sh: the kit does not unpack into the clone" >&2; exit 1; }
[ -z "$ROOTOWN" ] || chown -R root:wheel "$KROOT"
chmod -R go-w "$KROOT"
run install $ROOTOWN -m 0444 "$SHAFILE" "$KROOT/SHA256-v5.txt"
run install $ROOTOWN -m 0555 "$ASRC/summarize.sh" "$KROOT/summarize.sh"
run install $ROOTOWN -m 0555 "$CUDASRC" "$KROOT/run-linux-cuda.sh"
run install $ROOTOWN -m 0555 "$ASRC/testboot-return" "$MNT/usr/local/sbin/testboot-return"
run install $ROOTOWN -m 0555 "$ASRC/nvidia_client_owner_testboot" "$MNT/usr/local/etc/rc.d/nvidia_client_owner_testboot"
printf '%s\n' "$COMMIT" > "$MNT/usr/local/etc/nvidia_client_owner_testboot.version"

# every other test-boot kit of the clone is turned off (a second kit would load other modules and reboot on its own)
turn_off() {
	_f=$1
	[ -f "$_f" ] && [ ! -L "$_f" ] || return 0
	grep -Eq '^[[:space:]]*[A-Za-z0-9_]+_testboot_enable=' "$_f" || return 0
	sed -E -e '/^[[:space:]]*nvidia_client_owner_testboot_enable=/b' \
	    -e 's/^([[:space:]]*[A-Za-z0-9_]+_testboot_enable)=.*/\1="NO"/' "$_f" > "$_f.nvco-new" || { rm -f "$_f.nvco-new"; return 1; }
	if cmp -s "$_f" "$_f.nvco-new"; then rm -f "$_f.nvco-new"; else cat "$_f.nvco-new" > "$_f" && rm -f "$_f.nvco-new"; echo "turned off the other test-boot kits in $_f"; fi
}
for _n in etc/rc.conf etc/rc.conf.local usr/local/etc/rc.conf usr/local/etc/rc.conf.local; do turn_off "$MNT/$_n"; done
for _n in etc/rc.conf.d usr/local/etc/rc.conf.d; do
	[ -d "$MNT/$_n" ] || continue
	find "$MNT/$_n" -type f | while IFS= read -r _g; do turn_off "$_g"; done
done
printf 'nvidia_client_owner_testboot_enable="YES"\nnvidia_client_owner_testboot_normal_be="%s"\nnvidia_client_owner_testboot_copyout="%s"\nnvidia_client_owner_testboot_cudarun="/usr/local/share/nvidia-client-owner-test/run-linux-cuda.sh"\n' \
    "$NORMAL" "$WORK/logs/uvm-2373-testboot" > "$MNT/etc/rc.conf.d/nvidia_client_owner_testboot"
# loader.conf of the clone only
printf '\n# nvidia client owner test boot (Track #2373): a panic restarts in 5 seconds and never waits in the debugger\nkern.panic_reboot_wait_time="5"\ndebug.debugger_on_panic="0"\n' >> "$MNT/boot/loader.conf"
grep -q '^kern.panic_reboot_wait_time="5"$' "$MNT/boot/loader.conf" && grep -q '^debug.debugger_on_panic="0"$' "$MNT/boot/loader.conf" \
    || { echo "make-be.sh: the loader.conf of the clone lacks the panic settings" >&2; exit 1; }

# 5. done
run $BECTL umount "$BE"
BE_DONE=1
if [ "$ACTIVATE" -eq 1 ]; then
	run $BECTL activate -t "$BE" || { echo "make-be.sh: $BE is built but NOT armed (bectl activate failed); retry with: bectl activate -t $BE, or remove it with: bectl destroy $BE" >&2; exit 1; }
	echo "armed: $BE boots once; the kit returns to $NORMAL by itself; results in /var/db/nvidia-client-owner-test/<stamp>/ and copied to $WORK/logs/uvm-2373-testboot/<stamp>/; read summary.txt there"
else
	echo "built, NOT armed (dry run): $BE; the base is $BASE, the host returns to $NORMAL"
	echo "arm it with: bectl activate -t $BE   (only when no Fable agent runs on this host: the boot reboots it)"
fi
