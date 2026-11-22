#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# check-be.sh - the mount check of the RM client ownership test boot environment (Track #2373): looks into a built
# boot environment (mounted by this script with bectl, or an already mounted tree) and says whether it will do what
# the kit says.  Run it by hand on every built boot environment before arming it; make-be.sh does not run it.
# It reads files only: nothing is loaded, booted or written (bectl mounts read-write, so atime may change).
#
# Usage: check-be.sh [-s SHA256FILE] [-c COMMIT] [-U] [-m MOUNTED_ROOT | BE]
#   BE          a boot environment to mount with bectl (run as root)
#   -m ROOT     check the tree already mounted at ROOT instead (no bectl)
#   -s FILE     the hash list the boot environment was built from (default $GB10_WORK/logs/uvm-2373/SHA256-v6.txt;
#               without -s, GB10_WORK must be set)
#   -c COMMIT   the commit the kit must record (default: HEAD of the repository this script is in)
#   -U          do not check that the files are owned by root (the tests install without root; only with -m)
#
# Checks, each printed "ok" or "FAIL"; exit 0 only when every one is ok, 1 when one failed, 2 on a usage error:
#   - hashes: nvidia.ko, nvidia-modeset.ko, nvidia-uvm.ko and the firmware module nvidia_gsp_ga10x_fw.ko each equal
#     the full literal sha256 the list gives their staged file, are root-owned mode 0444, and are not symbolic links;
#     the list holds exactly five entries (the four modules and the firmware), the installed copy of the list equals
#     the one given, and nvidia-drm.ko is not installed;
#   - loader.conf: kern.panic_reboot_wait_time="5" and debug.debugger_on_panic="0", and no loader.conf, rc.conf or
#     their .d directories load an nvidia module by themselves, and no nvidia module is in /boot/modules or /boot/kernel;
#   - commit: the version file holds the commit, and the installed rc script, summarize.sh, testboot-return and
#     check-be.sh are the ones of that commit;
#   - dead-man (static): the installed rc script arms the dead-man timer (shutdown -r) before its first kldload and
#     gates the loads on it, no rc.conf of the clone sets the timer to 0 or to a non-number, the default is a positive
#     number of minutes, and shutdown(8) is in the clone;
#   - order: in the rc script the firmware kldload comes before the firmware-loaded gate, which comes before the load
#     of nvidia.ko, and the root open of the GPU comes before the probe.
# The dead-man proof is a static one (the script text and settings); the live proof is the one-shot's own deadman row.

set -u
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
LC_ALL=C
export PATH LC_ALL
KITDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$KITDIR/../../.." && pwd)
SHAFILE=
COMMIT=
MNT=
NOOWNER=0
usage() { sed -n '5,/^# Checks/p' "$0" | sed '$d; s/^# \{0,1\}//' >&2; exit 2; }
while [ $# -gt 0 ]; do
	case $1 in
	-s) [ $# -ge 2 ] || usage; SHAFILE=$2; shift 2 ;;
	-c) [ $# -ge 2 ] || usage; COMMIT=$2; shift 2 ;;
	-m) [ $# -ge 2 ] || usage; MNT=$2; shift 2 ;;
	-U) NOOWNER=1; shift ;;
	-h | --help) usage ;;
	-*) echo "check-be.sh: unknown argument: $1" >&2; usage ;;
	*) break ;;
	esac
done
BE=${1:-}
[ -n "$MNT" ] || [ -n "$BE" ] || usage
[ -z "$MNT" ] || [ -z "$BE" ] || { echo "check-be.sh: give a boot environment or -m ROOT, not both" >&2; exit 2; }
[ "$NOOWNER" = 0 ] || [ -n "$MNT" ] || { echo "check-be.sh: -U is only for a tree given with -m" >&2; exit 2; }
if [ -z "$SHAFILE" ]; then
	[ -n "${GB10_WORK:-}" ] || { echo "check-be.sh: no hash list: give -s FILE or set GB10_WORK (the list is \$GB10_WORK/logs/uvm-2373/SHA256-v6.txt)" >&2; exit 2; }
	SHAFILE=$GB10_WORK/logs/uvm-2373/SHA256-v6.txt
fi
[ -f "$SHAFILE" ] || { echo "check-be.sh: no hash list $SHAFILE" >&2; exit 2; }
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_COMMON_DIR
git_() { git -c safe.directory="$REPO" --no-optional-locks -C "$REPO" "$@"; }
case ${COMMIT:-HEAD} in -*) echo "check-be.sh: the commit must not start with a dash" >&2; exit 2 ;; esac
COMMIT=$(git_ rev-parse --verify --end-of-options "${COMMIT:-HEAD}^{commit}" 2> /dev/null) || { echo "check-be.sh: the commit does not name a commit of $REPO (-c)" >&2; exit 2; }

MOUNTED=0
MNTBASE=
cleanup() {
	if [ "$MOUNTED" = 1 ]; then bectl umount "$BE" > /dev/null 2>&1; fi
	[ -z "$MNTBASE" ] || { rmdir "$MNT" 2> /dev/null; rmdir "$MNTBASE" 2> /dev/null; }
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if [ -z "$MNT" ]; then
	[ "$(id -u)" = 0 ] || { echo "check-be.sh: run as root to mount $BE (or use -m ROOT)" >&2; exit 2; }
	MNTBASE=$(mktemp -d /var/run/nvco-check.XXXXXXXX) || exit 2
	MNT=$MNTBASE/root
	mkdir "$MNT" || exit 2
	MOUNTED=1
	bectl mount "$BE" "$MNT" > /dev/null || { MOUNTED=0; echo "check-be.sh: cannot mount $BE" >&2; exit 2; }
fi

rc=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; rc=1; }
MODS=$MNT/boot/modules.nvco-test
KROOT=$MNT/usr/local/share/nvidia-client-owner-test
RCS=$MNT/usr/local/etc/rc.d/nvidia_client_owner_testboot
echo "check-be.sh: ${BE:-$MNT}, commit $COMMIT, list $SHAFILE"

literal() {	# literal <staged name>: the single full 64-digit hash the list gives it
	awk -v n="($1)" '$1 == "SHA256" && $2 == n && $3 == "=" && length($4) == 64 && $4 ~ /^[0-9a-f]+$/ { print $4; c++ } END { exit (c == 1 ? 0 : 1) }' "$SHAFILE"
}
# a path of the clone that is, or lies below, a symbolic link would be read from the host while the check runs and from
# the clone when it boots: the kit installs none, so any one is a deviation
linked() {	# linked <path relative to the clone>: succeeds when the path or one of its parents is a symbolic link
	_acc=$MNT
	for _c in $(echo "$1" | tr / ' '); do
		_acc=$_acc/$_c
		[ ! -L "$_acc" ] || return 0
	done
	return 1
}
for _rel in boot/loader.conf boot/modules.nvco-test usr/local/share/nvidia-client-owner-test usr/local/etc/rc.d/nvidia_client_owner_testboot usr/local/etc/nvidia_client_owner_testboot.version usr/local/sbin/testboot-return etc/rc.conf.d/nvidia_client_owner_testboot sbin/shutdown; do
	if linked "$_rel"; then bad "$_rel is, or lies below, a symbolic link of the clone: the check would read the host's file"; fi
done
[ -z "$(find "$MODS" "$KROOT" -type l 2> /dev/null)" ] || bad "a symbolic link lies in /boot/modules.nvco-test or in the kit directory of the clone"

# the list: five entries, the drm one among them
[ "$(grep -c '^SHA256 (' "$SHAFILE")" = 5 ] && ok "the list holds five entries" || bad "the list $SHAFILE does not hold exactly five SHA256 entries"
literal nvidia-drm-v5.ko > /dev/null && ok "the list has a full entry for nvidia-drm-v5.ko (checked, never installed)" || bad "the list has no single full 64-digit entry for nvidia-drm-v5.ko"

# firmware and hashes
for _p in nvidia-v6-phase0.ko:nvidia.ko nvidia-modeset-v5.ko:nvidia-modeset.ko nvidia-uvm-v5.ko:nvidia-uvm.ko nvidia_gsp_ga10x_fw-v5.ko:nvidia_gsp_ga10x_fw.ko; do
	_s=${_p%%:*}; _d=${_p#*:}
	_want=$(literal "$_s") || { bad "the list has no single full 64-digit entry for $_s"; continue; }
	if [ ! -f "$MODS/$_d" ]; then bad "$_d is not in /boot/modules.nvco-test"; continue; fi
	_got=$(sha256 -q "$MODS/$_d")
	if [ "$_got" = "$_want" ]; then ok "$_d is $_got (the list's $_s)"; else bad "$_d is $_got, the list says $_want"; fi
	_mode=$(stat -f %Lp "$MODS/$_d"); _own=$(stat -f %u "$MODS/$_d")
	if [ "$NOOWNER" = 1 ]; then
		[ "$_mode" = 444 ] && ok "$_d is mode 0444 (owner NOT checked: -U)" || bad "$_d is mode $_mode, not 0444"
	else
		[ "$_mode $_own" = "444 0" ] && ok "$_d is root-owned, mode 0444" || bad "$_d is mode $_mode, owner uid $_own: it must be root-owned, mode 0444"
	fi
done
[ ! -e "$MODS/nvidia-drm.ko" ] && ok "nvidia-drm.ko is not installed" || bad "nvidia-drm.ko is installed"
if [ ! -f "$KROOT/SHA256-v6.txt" ]; then bad "the clone has no SHA256-v6.txt in its kit directory (a clone built before the v6 kit, or a damaged one)"
elif cmp -s "$SHAFILE" "$KROOT/SHA256-v6.txt"; then ok "the installed hash list is the given one"
else bad "the installed hash list differs from $SHAFILE"; fi

# loader.conf of the clone
if grep -q '^kern.panic_reboot_wait_time="5"$' "$MNT/boot/loader.conf" 2> /dev/null && grep -q '^debug.debugger_on_panic="0"$' "$MNT/boot/loader.conf" 2> /dev/null; then ok "loader.conf has the panic settings (5 seconds, no debugger)"; else bad "loader.conf lacks kern.panic_reboot_wait_time=\"5\" or debug.debugger_on_panic=\"0\""; fi
# nothing loads an nvidia module by itself: the files make-be.sh scans, a backslash-continued line joined first
selfload=
for _f in boot/loader.conf boot/loader.conf.local etc/rc.conf etc/rc.conf.local usr/local/etc/rc.conf usr/local/etc/rc.conf.local; do
	[ -f "$MNT/$_f" ] && selfload="$selfload $MNT/$_f"
done
for _d in boot/loader.conf.d etc/rc.conf.d usr/local/etc/rc.conf.d; do
	[ -d "$MNT/$_d" ] && selfload="$selfload $(find "$MNT/$_d" -type f | tr '\n' ' ')"
done
_hit=
for _f in $selfload; do
	if sed -e :a -e '/\\$/N; s/\\\n//; ta' "$_f" 2> /dev/null | grep -Eq '^[[:space:]]*(kld_list\+?=.*nvidia|nvidia[A-Za-z0-9_-]*_load[[:space:]]*=[[:space:]]*"?[Yy])'; then _hit="$_hit $_f"; fi
done
[ -z "$_hit" ] && ok "no loader.conf, rc.conf or .d file loads an nvidia module by itself" || bad "these files load an nvidia module by themselves:$_hit"
_pk=$(ls "$MNT"/boot/modules/nvidia*.ko "$MNT"/boot/kernel/nvidia*.ko 2> /dev/null)
[ -z "$_pk" ] && ok "no nvidia module is in /boot/modules or /boot/kernel" || bad "an nvidia module is in /boot/modules or /boot/kernel: $_pk"

# the commit
_v=$(cat "$MNT/usr/local/etc/nvidia_client_owner_testboot.version" 2> /dev/null || echo none)
[ "$_v" = "$COMMIT" ] && ok "the clone records commit $_v" || bad "the clone records '$_v', not $COMMIT"
for _f in nvidia_client_owner_testboot:"$RCS" summarize.sh:"$KROOT/summarize.sh" testboot-return:"$MNT/usr/local/sbin/testboot-return" check-be.sh:"$KROOT/repo/tests/ioctl/testboot/check-be.sh" gpu-detect.sh:"$KROOT/repo/tests/ioctl/testboot/gpu-detect.sh"; do
	_n=${_f%%:*}; _dst=${_f#*:}
	if [ ! -f "$_dst" ]; then bad "$_n is not installed in the clone ($_dst)"; continue; fi
	if ! git_ cat-file -e "$COMMIT:tests/ioctl/testboot/$_n" 2> /dev/null; then bad "$_n is not in commit $COMMIT of $REPO: run this script from a checkout that has the commit"; continue; fi
	if git_ show "$COMMIT:tests/ioctl/testboot/$_n" 2> /dev/null | cmp -s - "$_dst"; then ok "$_n is the one of commit $COMMIT"; else bad "$_n differs from the one of commit $COMMIT"; fi
done

# the dead-man proof (static) and the order of the steps
lineof() { grep -n "$1" "$RCS" | grep -v '^[0-9]*:[[:space:]]*#' | head -1 | cut -d: -f1; }
if [ -f "$RCS" ]; then
	_arm=$(lineof '^	deadman_arm$')
	_first=$(lineof 'kldload ')
	_gate=$(lineof 'if \[ "\$DM_OK" != 1 \]; then ok=0')
	if [ -z "$_arm" ]; then bad "dead-man: the rc script has no deadman_arm call"
	elif [ -z "$_first" ]; then bad "dead-man: the rc script has no kldload line to compare with"
	elif [ "$_arm" -ge "$_first" ]; then bad "dead-man: the timer is armed (line $_arm) after the first kldload (line $_first)"
	else ok "dead-man: the rc script arms the timer (line $_arm) before its first kldload (line $_first)"; fi
	if [ -z "$_gate" ]; then bad "dead-man: the rc script has no line that skips the loads when the timer is not pending"
	elif [ -n "$_first" ] && [ "$_gate" -ge "$_first" ]; then bad "dead-man: the gate on the timer (line $_gate) is after the first kldload (line $_first)"
	else ok "dead-man: a timer that is not pending loads nothing (gate at line $_gate, before the first kldload)"; fi
	grep -q 'shutdown -r "+\$nvidia_client_owner_testboot_deadman"' "$RCS" && ok "dead-man: the timer is shutdown -r +N" || bad "dead-man: no shutdown -r request in the rc script"
	_def=$(sed -n 's/^: "\${nvidia_client_owner_testboot_deadman:=\([0-9]*\)}"$/\1/p' "$RCS")
	case $_def in '' | 0*) bad "dead-man: the default is '$_def', not a positive number of minutes" ;; *) ok "dead-man: the default is $_def minutes" ;; esac
	# every place rc.subr reads for this service: the rc.conf files, and etc and usr/local/etc rc.conf.d/<name> (a file, or a directory of files)
	_dmfiles=
	for _f in etc/rc.conf etc/rc.conf.local usr/local/etc/rc.conf usr/local/etc/rc.conf.local; do [ -f "$MNT/$_f" ] && _dmfiles="$_dmfiles $MNT/$_f"; done
	for _d in etc/rc.conf.d/nvidia_client_owner_testboot usr/local/etc/rc.conf.d/nvidia_client_owner_testboot; do
		if [ -d "$MNT/$_d" ]; then _dmfiles="$_dmfiles $(find "$MNT/$_d" -type f | tr '\n' ' ')"; elif [ -f "$MNT/$_d" ]; then _dmfiles="$_dmfiles $MNT/$_d"; fi
	done
	_baddm=
	for _f in $_dmfiles; do
		for _val in $(sed -n 's/^[[:space:]]*nvidia_client_owner_testboot_deadman[[:space:]]*=[[:space:]]*"\{0,1\}\([^"#[:space:]]*\).*$/V\1/p' "$_f"); do
			case ${_val#V} in '' | *[!0-9]* | 0*) _baddm="$_baddm $_f='${_val#V}'" ;; esac
		done
	done
	[ -z "$_baddm" ] && ok "dead-man: no rc.conf of the clone sets the timer to 0 or to a non-number" || bad "dead-man: the timer is set to 0 or to a non-number in:$_baddm"
	[ -x "$MNT/sbin/shutdown" ] && ok "dead-man: shutdown(8) is in the clone" || bad "dead-man: no shutdown(8) in the clone"
	_fw=$(lineof 'kldload "\$MODS/nvidia_gsp_ga10x_fw.ko"')
	_fg=$(lineof 'if kldstat -q -n nvidia_gsp_ga10x_fw')
	_nv=$(lineof 'for _k in nvidia ')
	if [ -z "$_fw" ]; then bad "order: no kldload of the firmware module in the rc script"
	elif [ -z "$_fg" ]; then bad "order: no kldstat -q -n nvidia_gsp_ga10x_fw gate in the rc script"
	elif [ -z "$_nv" ]; then bad "order: no load loop for nvidia.ko in the rc script"
	elif [ "$_fw" -ge "$_fg" ] || [ "$_fg" -ge "$_nv" ]; then bad "order: the firmware kldload (line $_fw), its gate (line $_fg) and the load of nvidia.ko (line $_nv) are not in that order"
	else ok "order: the firmware is loaded (line $_fw), checked (line $_fg), then nvidia.ko is loaded (line $_nv)"; fi
	_ro=$(lineof 'running root-open ')
	_ob=$(lineof 'running owner-boot ')
	if [ -z "$_ro" ]; then bad "order: no root-open step in the rc script"
	elif [ -z "$_ob" ]; then bad "order: no owner-boot step in the rc script to compare the root open with"
	elif [ "$_ro" -ge "$_ob" ]; then bad "order: the root open (line $_ro) is not before the probe (line $_ob)"
	else ok "order: root opens the GPU (line $_ro) before the probe runs (line $_ob)"; fi
else
	bad "dead-man: the rc script is not installed"
fi
grep -q '^nvidia_client_owner_testboot_enable="YES"$' "$MNT/etc/rc.conf.d/nvidia_client_owner_testboot" 2> /dev/null && ok "the one-shot is enabled in the clone" || bad "the one-shot is not enabled in the clone"

if [ "$rc" = 0 ]; then echo "check-be.sh: all checks passed"; else echo "check-be.sh: FAILED: do not arm this boot environment; fix what failed above, or destroy it and build a new one with make-be.sh"; fi
exit $rc
