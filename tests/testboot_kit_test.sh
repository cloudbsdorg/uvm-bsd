#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# testboot_kit_test.sh - Track #2373: the one-shot test-boot kit of the RM client ownership (tests/ioctl/testboot):
# make-be.sh on a fake bectl (a dry run builds a boot environment and never arms it; the hash checks refuse a
# changed, short or missing entry; a base that loads nvidia is refused and cleaned up; the panic settings and the
# root-owned module directory are in the clone; what is installed is the git archive of the commit), the rc
# one-shot on fake commands (testboot-return is ALWAYS the last step, linux64 is loaded first, nvidia-drm never),
# and the reading rule of summarize.sh on fixtures.  Plants (deliberately broken copies) must turn the matching
# check red.
#
# Run: sh tests/testboot_kit_test.sh

set -u
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
LC_ALL=C
export PATH LC_ALL
TESTSDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$TESTSDIR/.." && pwd)
KIT=$REPO/tests/ioctl/testboot
WORK=${GB10_WORK:-$HOME/work/gb10}
mkdir -p "$WORK/tmp"
T=$(mktemp -d "$WORK/tmp/testboot-kit.XXXXXXXX")
trap 'rm -rf "$T"' EXIT HUP INT QUIT TERM
rc=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; rc=1; }

# ---- fixtures: staged modules and their list
STAGE=$T/stage
mkdir -p "$STAGE"
for f in nvidia-v5-phase0.ko nvidia-modeset-v5.ko nvidia-drm-v5.ko nvidia-uvm-v5.ko; do printf 'module %s\n' "$f" > "$STAGE/$f"; done
( cd "$STAGE" && sha256 nvidia-v5-phase0.ko nvidia-modeset-v5.ko nvidia-drm-v5.ko nvidia-uvm-v5.ko > SHA256-v5.txt )
cp "$STAGE/SHA256-v5.txt" "$T/list.save"
cp "$STAGE/nvidia-uvm-v5.ko" "$T/uvm.save"
GBW=$T/gbwork
mkdir -p "$GBW/tmp" "$GBW/nvidia-gb10-freebsd-uvm0/scripts"
printf '#!/bin/sh\necho cuda\n' > "$GBW/nvidia-gb10-freebsd-uvm0/scripts/run-linux-cuda.sh"

# ---- fake bectl: state in $T/be, the base tree in $T/basetree
mkdir -p "$T/be" "$T/basetree/etc/rc.conf.d" "$T/basetree/boot" "$T/basetree/usr/local/etc"
base_tree() {
	rm -rf "$T/basetree"; mkdir -p "$T/basetree/etc/rc.conf.d" "$T/basetree/boot" "$T/basetree/usr/local/etc"
	printf 'hostname="x"\nkld_list="filemon amdgpu"\nprime_import_testboot_enable="YES"\n' > "$T/basetree/etc/rc.conf"
	printf 'foo_testboot_enable="YES"\n' > "$T/basetree/etc/rc.conf.d/other"
	printf 'zfs_load="YES"\n' > "$T/basetree/boot/loader.conf"
}
base_tree
cat > "$T/bectl" <<'EOF'
#!/bin/sh
S=$FAKE_BE
echo "bectl $*" >> "$S.log"
case $1 in
create) [ "$2" = -e ] && { mkdir "$S/$4" || exit 1; } ;;
mount) rm -rf "$3"; mkdir -p "$3" && cp -R "$FAKE_BASETREE/." "$3/" ;;
umount) : ;;
destroy) rm -rf "$S/$3" ;;
activate) [ -z "${FAKE_ACTIVATE_FAIL:-}" ] || exit 1 ;;
esac
exit 0
EOF
chmod +x "$T/bectl"
export GB10_TEST_HOOKS=1 NVCO_BECTL=$T/bectl FAKE_BE=$T/be FAKE_BASETREE=$T/basetree NVCO_NOT_ROOT=1 NVCO_NO_GIT_CHECK=1 GB10_WORK=$GBW

with() {	# with VAR=value command...: the variable is exported for that command only
	( export "$1"; shift; "$@" )
}
mk() {	# mk <mnt> <args...>: run make-be.sh with fixtures; output in $T/mk.out
	_mnt=$1; shift
	: > "$T/be.log"
	NVCO_MNT=$_mnt sh "$KIT/make-be.sh" -m "$STAGE" -D 20261122 -n 1 "$@" > "$T/mk.out" 2>&1
}

# ---- 1. a dry run builds the clone and does not arm it
mk "$T/mnt1/root"; r=$?
if [ "$r" = 0 ]; then ok "dry run exits 0"; else cat "$T/mk.out"; bad "dry run exit $r"; fi
if grep -q '^bectl activate' "$T/be.log"; then bad "a dry run called bectl activate"; else ok "a dry run never calls bectl activate"; fi
grep -qF 'arm it with: bectl activate -t nvidia-client-owner-test-20261122-1' "$T/mk.out" && ok "the arm command is printed" || bad "the arm command is not printed"
grep -q 'no Fable agent runs on this host' "$T/mk.out" && ok "the arm command carries the no-Fable condition" || bad "the arm command lacks the no-Fable condition"
M=$T/mnt1/root
for pair in nvidia-v5-phase0.ko:nvidia.ko nvidia-modeset-v5.ko:nvidia-modeset.ko nvidia-uvm-v5.ko:nvidia-uvm.ko; do
	s=${pair%%:*}; d=${pair#*:}
	if [ -f "$M/boot/modules.nvco-test/$d" ] && [ "$(sha256 -q "$M/boot/modules.nvco-test/$d")" = "$(sha256 -q "$STAGE/$s")" ]; then ok "$d is in the clone with the staged hash"; else bad "$d is not in the clone with the staged hash"; fi
	[ "$(stat -f %Lp "$M/boot/modules.nvco-test/$d")" = 444 ] && ok "$d is mode 0444" || bad "$d is not mode 0444"
done
[ ! -e "$M/boot/modules.nvco-test/nvidia-drm.ko" ] && ok "nvidia-drm.ko is not installed" || bad "nvidia-drm.ko is installed"
grep -q '^kern.panic_reboot_wait_time="5"$' "$M/boot/loader.conf" && grep -q '^debug.debugger_on_panic="0"$' "$M/boot/loader.conf" && ok "the loader.conf of the clone has the panic settings" || bad "the loader.conf of the clone lacks the panic settings"
grep -q 'kern.panic' "$T/basetree/boot/loader.conf" && bad "the base tree was changed" || ok "the base loader.conf is untouched"
RCD=$M/etc/rc.conf.d/nvidia_client_owner_testboot
grep -q '^nvidia_client_owner_testboot_enable="YES"$' "$RCD" && ok "the one-shot is enabled in the clone" || bad "the one-shot is not enabled"
grep -q '^nvidia_client_owner_testboot_normal_be="cloudbsd-rx7600"$' "$RCD" && ok "the normal boot environment is cloudbsd-rx7600" || bad "the normal boot environment is not recorded"
grep -q "^nvidia_client_owner_testboot_copyout=\"$GBW/logs/uvm-2373-testboot\"\$" "$RCD" && ok "the copy-out directory follows GB10_WORK" || bad "the copy-out directory is not written to rc.conf.d"
grep -q '^nvidia_client_owner_testboot_cudarun="/usr/local/share/nvidia-client-owner-test/run-linux-cuda.sh"$' "$RCD" && [ -x "$M/usr/local/share/nvidia-client-owner-test/run-linux-cuda.sh" ] && ok "the CUDA script is installed in the kit and used from there" || bad "the CUDA script is not in the kit"
grep -q '^prime_import_testboot_enable="NO"$' "$M/etc/rc.conf" && ok "another test-boot kit is turned off in rc.conf" || bad "another test-boot kit stays on in rc.conf"
grep -q '^foo_testboot_enable="NO"$' "$M/etc/rc.conf.d/other" && ok "another test-boot kit is turned off in rc.conf.d" || bad "another test-boot kit stays on in rc.conf.d"
cmp -s "$KIT/testboot-return" "$M/usr/local/sbin/testboot-return" && ok "testboot-return is installed" || bad "testboot-return is not installed"
[ -f "$M/usr/local/share/nvidia-client-owner-test/repo/tests/ioctl/run_owner_boot.sh" ] && [ -f "$M/usr/local/share/nvidia-client-owner-test/SHA256-v5.txt" ] && ok "the kit and the hash list are in the clone" || bad "the kit or the hash list is missing from the clone"
grep -q 'nvidia-client-owner-test-20261122-1' "$T/be.log" && ok "the clone is named nvidia-client-owner-test-<date>-<n>" || bad "the clone name is wrong"
[ ! -e "$T/mnt1/kit.tar" ] && ok "the scratch archive is removed" || bad "the scratch archive is left"

# ---- 2. --activate arms; a failed activation says so
mk "$T/mnt2/root" -n 2 --activate; r=$?
[ "$r" = 0 ] && grep -q '^bectl activate -t nvidia-client-owner-test-20261122-2$' "$T/be.log" && ok "--activate runs bectl activate -t" || { cat "$T/mk.out"; bad "--activate did not arm"; }
with FAKE_ACTIVATE_FAIL=1 mk "$T/mnt2b/root" -n 12 --activate; r=$?
[ "$r" != 0 ] && grep -q 'built but NOT armed' "$T/mk.out" && ok "a failed activation says the boot environment is built but not armed" || bad "a failed activation is not reported"

# ---- 3. refusals
refuse() {	# refuse <name> <n> <args...>: make-be.sh must fail and leave no boot environment of that name
	_n=$1; _seq=$2; shift 2
	mk "$T/mnt-$_n/root" -n "$_seq" "$@"; r=$?
	if [ "$r" != 0 ]; then ok "refused: $_n"; else bad "not refused: $_n"; fi
	[ ! -d "$T/be/nvidia-client-owner-test-20261122-$_seq" ] && ok "no half-built boot environment: $_n" || bad "a boot environment is left: $_n"
}
printf 'tampered\n' > "$STAGE/nvidia-uvm-v5.ko"
refuse changed-module 3
cp "$T/uvm.save" "$STAGE/nvidia-uvm-v5.ko"
sed 's/^\(SHA256 (nvidia-v5-phase0.ko) = [0-9a-f]\{63\}\)[0-9a-f]$/\1/' "$T/list.save" > "$STAGE/SHA256-v5.txt"
cmp -s "$T/list.save" "$STAGE/SHA256-v5.txt" && bad "the short-hash fixture changed nothing"
refuse short-hash 4
sed 's/^\(SHA256 (nvidia-v5-phase0.ko) = \)\([0-9a-f]\{8\}\).*$/\1\2/' "$T/list.save" > "$STAGE/SHA256-v5.txt"
refuse prefix-hash 5
grep -v 'nvidia-drm-v5.ko' "$T/list.save" > "$STAGE/SHA256-v5.txt"
refuse missing-entry 6
cp "$T/list.save" "$STAGE/SHA256-v5.txt"
printf 'kld_list="filemon amdgpu nvidia"\n' > "$T/basetree/etc/rc.conf"
refuse base-kld-list-nvidia 7
base_tree
printf 'nvidia_load="YES"\n' >> "$T/basetree/boot/loader.conf"
refuse base-loader-loads-nvidia 13
base_tree
mkdir -p "$T/basetree/boot/loader.conf.d"; printf 'nvidia-modeset_load="YES"\n' > "$T/basetree/boot/loader.conf.d/x.conf"
refuse base-loader-conf-d-loads-nvidia 14
base_tree
mkdir -p "$T/basetree/usr/local/etc/rc.conf.d"; printf 'kld_list="nvidia"\n' > "$T/basetree/usr/local/etc/rc.conf.d/kld"
refuse base-usr-local-rc-conf-d-loads-nvidia 26
base_tree
mkdir -p "$T/basetree/boot/modules"; : > "$T/basetree/boot/modules/nvidia-uvm.ko"
refuse base-has-packaged-nvidia-module 15
base_tree
mkdir -p "$T/basetree/boot/modules.nvco-test"
refuse modules-dir-exists 8
base_tree
# a mount point that is a symbolic link is refused
mkdir -p "$T/elsewhere"; mkdir -p "$T/mntlink"; ln -s "$T/elsewhere" "$T/mntlink/root"
mk "$T/mntlink/root" -n 16; r=$?
if [ "$r" != 0 ]; then ok "refused: mount-point-is-a-link"; else bad "not refused: mount-point-is-a-link"; fi
[ ! -d "$T/be/nvidia-client-owner-test-20261122-16" ] && ok "no half-built boot environment: mount-point-is-a-link" || bad "a boot environment is left: mount-point-is-a-link"
# the vendored testboot-return must be the pinned one
cp -R "$KIT" "$T/kitcopy"
printf '\n# changed\n' >> "$T/kitcopy/testboot-return"
mkdir -p "$T/repocopy/tests/ioctl" "$T/repocopy/sys"; cp -R "$T/kitcopy" "$T/repocopy/tests/ioctl/testboot"
: > "$T/be.log"
NVCO_MNT=$T/mnt-vendor/root sh "$T/repocopy/tests/ioctl/testboot/make-be.sh" -m "$STAGE" -D 20261122 -n 9 > "$T/mk.out" 2>&1 && bad "a changed vendored testboot-return was accepted" || ok "a changed vendored testboot-return is refused"
# the test hooks are not honoured without GB10_TEST_HOOKS: NVCO_MNT must not reach bectl mount
: > "$T/be.log"
( unset GB10_TEST_HOOKS; NVCO_MNT=$T/mnt-nohook/root sh "$KIT/make-be.sh" -m "$STAGE" -D 20261122 -n 17 > "$T/mk.out" 2>&1 )
grep -q 'bectl mount' "$T/be.log" && bad "make-be.sh went on without root and without the test hooks" || ok "without GB10_TEST_HOOKS a non-root run is refused before bectl"

# ---- what is installed is the git archive of the commit; a dirty kit is refused (a real repository fixture)
GR=$T/gitrepo
mkdir -p "$GR/tests/ioctl" "$GR/sys"; cp -R "$KIT" "$GR/tests/ioctl/testboot"; cp "$REPO/tests/ioctl/run_owner_boot.sh" "$REPO/tests/ioctl/uvm_owner_race.c" "$GR/tests/ioctl/"; printf 'x\n' > "$GR/sys/uvm_main.c"
printf 'tests/ioctl/planted\n' > "$GR/.gitignore"
gitq() { GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null git -c init.templateDir= -c user.name=t -c user.email=t@t -C "$GR" "$@"; }
gitq init -q && gitq add -A && gitq commit -q -m fixture
printf 'planted\n' > "$GR/tests/ioctl/planted"
unset NVCO_NO_GIT_CHECK
: > "$T/be.log"
GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null NVCO_MNT=$T/mnt-git/root sh "$GR/tests/ioctl/testboot/make-be.sh" -m "$STAGE" -D 20261122 -n 18 > "$T/mk.out" 2>&1; r=$?
if [ "$r" = 0 ]; then ok "a committed kit is built"; else cat "$T/mk.out"; bad "a committed kit was refused ($r)"; fi
KR=$T/mnt-git/root/usr/local/share/nvidia-client-owner-test/repo
[ ! -e "$KR/tests/ioctl/planted" ] && ok "a git-ignored file is not installed (the archive of the commit is)" || bad "a git-ignored file was installed"
[ "$(cat "$T/mnt-git/root/usr/local/etc/nvidia_client_owner_testboot.version")" = "$(gitq rev-parse HEAD)" ] && ok "the commit is recorded in the clone" || bad "the commit is not recorded"
printf '# edit\n' >> "$GR/tests/ioctl/run_owner_boot.sh"
GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null NVCO_MNT=$T/mnt-dirty/root sh "$GR/tests/ioctl/testboot/make-be.sh" -m "$STAGE" -D 20261122 -n 19 > "$T/mk.out" 2>&1 && bad "a kit with an uncommitted change was built" || ok "a kit with an uncommitted change is refused"
grep -q 'uncommitted changes' "$T/mk.out" && ok "and says so" || bad "the refusal does not say what is wrong"
# everything installed is the archive of the commit: the rc script, summarize.sh and testboot-return too
for f in nvidia_client_owner_testboot summarize.sh testboot-return; do
	case $f in nvidia_client_owner_testboot) dst=$T/mnt-git/root/usr/local/etc/rc.d/$f ;; testboot-return) dst=$T/mnt-git/root/usr/local/sbin/$f ;; *) dst=$T/mnt-git/root/usr/local/share/nvidia-client-owner-test/$f ;; esac
	gitq show "HEAD:tests/ioctl/testboot/$f" > "$T/committed.$f" 2>/dev/null
	cmp -s "$T/committed.$f" "$dst" && ok "the installed $f is the committed one" || bad "the installed $f is not the committed one"
done
# an edit of the work tree between the check and the install must not be installed: the dirty check is removed (a plant) and
# the committed rc script is still what lands; with the sources read from the work tree as well (a second plant) it is not
printf '# worktree edit\n' >> "$GR/tests/ioctl/testboot/nvidia_client_owner_testboot"
rm -rf "$T/gp-a" "$T/gp-b"
cp -R "$GR" "$T/gp-a"; cp -R "$GR" "$T/gp-b"
for g in a b; do
	sed -e 's/^\t\[ -z "\$_dirty" \] || {/\t[ -z "" ] || {/' "$GR/tests/ioctl/testboot/make-be.sh" > "$T/gp-$g/tests/ioctl/testboot/make-be.sh"
done
cmp -s "$GR/tests/ioctl/testboot/make-be.sh" "$T/gp-a/tests/ioctl/testboot/make-be.sh" && bad "the dirty-check plant changed nothing"
sed -e 's/^ASRC=\$SCRATCH\/kit\/tests\/ioctl\/testboot$/ASRC=$KITDIR/' "$T/gp-b/tests/ioctl/testboot/make-be.sh" > "$T/gp-b/tests/ioctl/testboot/mb.sh" && mv "$T/gp-b/tests/ioctl/testboot/mb.sh" "$T/gp-b/tests/ioctl/testboot/make-be.sh"
cmp -s "$T/gp-a/tests/ioctl/testboot/make-be.sh" "$T/gp-b/tests/ioctl/testboot/make-be.sh" && bad "the work-tree plant changed nothing"
GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null NVCO_MNT=$T/mnt-gpa/root sh "$T/gp-a/tests/ioctl/testboot/make-be.sh" -m "$STAGE" -D 20261122 -n 28 > "$T/mk.out" 2>&1 || { cat "$T/mk.out"; bad "the dirty-check plant run failed"; }
cmp -s "$T/committed.nvidia_client_owner_testboot" "$T/mnt-gpa/root/usr/local/etc/rc.d/nvidia_client_owner_testboot" && ok "with the dirty check off, the committed rc script is still what is installed" || { cat "$T/mk.out"; bad "an edit of the work tree reached the clone although the install reads the archive"; }
GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null NVCO_MNT=$T/mnt-gpb/root sh "$T/gp-b/tests/ioctl/testboot/make-be.sh" -m "$STAGE" -D 20261122 -n 29 > "$T/mk.out" 2>&1; _r=$?
if [ "$_r" = 0 ] && cmp -s "$T/gp-b/tests/ioctl/testboot/nvidia_client_owner_testboot" "$T/mnt-gpb/root/usr/local/etc/rc.d/nvidia_client_owner_testboot"; then ok "plant installs-from-work-tree is red (the edited work-tree script lands in the clone)"; else cat "$T/mk.out"; bad "plant installs-from-work-tree did not show the work-tree script in the clone (exit $_r)"; fi
rm -rf "$T/gp-a" "$T/gp-b"
export NVCO_NO_GIT_CHECK=1

# a stale scratch tree of an earlier run is not installed
mkdir -p "$T/mnt-stale/kit/tests/ioctl"; : > "$T/mnt-stale/kit/tests/ioctl/stale-file"
: > "$T/be.log"; NVCO_MNT=$T/mnt-stale/root sh "$KIT/make-be.sh" -m "$STAGE" -D 20261122 -n 30 > "$T/mk.out" 2>&1
[ -f "$T/mnt-stale/root/usr/local/share/nvidia-client-owner-test/repo/tests/ioctl/run_owner_boot.sh" ] && [ ! -e "$T/mnt-stale/root/usr/local/share/nvidia-client-owner-test/repo/tests/ioctl/stale-file" ] && ok "a stale file of an earlier scratch tree is not installed" || { cat "$T/mk.out"; bad "a stale scratch file reached the clone"; }
# a symbolic link in the kit is refused
mkdir -p "$T/symrepo/tests/ioctl" "$T/symrepo/sys"; cp -R "$KIT" "$T/symrepo/tests/ioctl/testboot"; ln -s /etc/passwd "$T/symrepo/tests/ioctl/evil"
NVCO_MNT=$T/mnt-sym/root sh "$T/symrepo/tests/ioctl/testboot/make-be.sh" -m "$STAGE" -D 20261122 -n 31 > "$T/mk.out" 2>&1 && bad "a kit with a symbolic link was built" || ok "a kit with a symbolic link is refused"
grep -q 'symbolic link' "$T/mk.out" && [ ! -e "$T/be/nvidia-client-owner-test-20261122-31" ] && ok "and nothing is built" || bad "the symbolic link refusal left a boot environment"
# a continued kld_list line is joined before the scan
printf 'kld_list="linux64 \\\n nvidia-modeset"\n' > "$T/contline"
base_tree; mkdir -p "$T/basetree/usr/local/etc/rc.conf.d"; cp "$T/contline" "$T/basetree/usr/local/etc/rc.conf.d/kld"
refuse base-continued-kld-list-loads-nvidia 32
base_tree
# a file missing from the commit is named in terms of the commit
rm -rf "$T/gm"; cp -R "$GR" "$T/gm"; gitm() { GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null git -c user.name=t -c user.email=t@t -C "$T/gm" "$@"; }
mkdir -p "$T/gm/.git/info"; gitm checkout -q -- . 2>/dev/null; gitm rm -q --cached tests/ioctl/testboot/summarize.sh && printf 'tests/ioctl/testboot/summarize.sh\n' >> "$T/gm/.git/info/exclude" && gitm commit -q -m drop
unset NVCO_NO_GIT_CHECK
GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null NVCO_MNT=$T/mnt-gm/root sh "$T/gm/tests/ioctl/testboot/make-be.sh" -m "$STAGE" -D 20261122 -n 33 > "$T/mk.out" 2>&1 && bad "a commit without summarize.sh was built" || ok "a commit without summarize.sh is refused"
grep -q 'commit .* has no tests/ioctl/testboot/summarize.sh' "$T/mk.out" && ok "and the message names the commit and the file" || { cat "$T/mk.out"; bad "the message is not in terms of the commit"; }
export NVCO_NO_GIT_CHECK=1
rm -rf "$T/gm"

# ---- plants on make-be.sh
# two independent checks of the same hash (the staged file, then the copy): either alone refuses the changed module;
# with both turned off a changed module is installed
plant_mk() {	# plant_mk <name> <seq> <expect: refused|accepted> <sed args...>
	_pn=$1; _pseq=$2; _pexp=$3; shift 3
	mkdir -p "$T/pk-$_pn/tests/ioctl" "$T/pk-$_pn/sys"
	cp -R "$KIT" "$T/pk-$_pn/tests/ioctl/testboot"
	sed "$@" "$KIT/make-be.sh" > "$T/pk-$_pn/tests/ioctl/testboot/make-be.sh"
	if cmp -s "$KIT/make-be.sh" "$T/pk-$_pn/tests/ioctl/testboot/make-be.sh"; then bad "plant $_pn changed nothing"; return; fi
	printf 'tampered\n' > "$STAGE/nvidia-uvm-v5.ko"
	NVCO_MNT=$T/mnt-pk-$_pn/root sh "$T/pk-$_pn/tests/ioctl/testboot/make-be.sh" -m "$STAGE" -D 20261122 -n "$_pseq" > "$T/mk.out" 2>&1 && r=0 || r=$?
	cp "$T/uvm.save" "$STAGE/nvidia-uvm-v5.ko"
	case $_pexp:$r in
	refused:0) bad "plant $_pn: the changed module was installed although the other check is left" ;;
	refused:*) ok "plant $_pn: the other hash check still refuses the changed module" ;;
	accepted:0) ok "plant $_pn is red (both hash checks off: the changed module is installed)" ;;
	*) bad "plant $_pn stays green" ;;
	esac
}
plant_mk source-check-off 20 refused -e '/MODDIR\/\$_f is/s/^\t\[ "\$_got" = "\$_want" \]/\ttrue/'
plant_mk copy-check-off 21 refused -e '/differs after the copy/s/^\t\[ "\$_got" = "\$_want" \]/\ttrue/'
plant_mk both-checks-off 22 accepted -e 's/^\t\[ "\$_got" = "\$_want" \] ||/\ttrue ||/'
plant_refused() {	# plant_refused <name> <seq> <setup fn> <sed args...>: the plant makes make-be.sh accept what it refused
	_pn=$1; _pseq=$2; _setup=$3; shift 3
	mkdir -p "$T/pk-$_pn/tests/ioctl" "$T/pk-$_pn/sys"; cp -R "$KIT" "$T/pk-$_pn/tests/ioctl/testboot"
	sed "$@" "$KIT/make-be.sh" > "$T/pk-$_pn/tests/ioctl/testboot/make-be.sh"
	if cmp -s "$KIT/make-be.sh" "$T/pk-$_pn/tests/ioctl/testboot/make-be.sh"; then bad "plant $_pn changed nothing"; return; fi
	"$_setup"
	NVCO_MNT=$T/mnt-pk-$_pn/root sh "$T/pk-$_pn/tests/ioctl/testboot/make-be.sh" -m "$STAGE" -D 20261122 -n "$_pseq" > "$T/mk.out" 2>&1 && r=0 || r=$?
	base_tree; cp "$T/list.save" "$STAGE/SHA256-v5.txt"
	if [ "$r" = 0 ]; then ok "plant $_pn is red (the refused case is accepted)"; else bad "plant $_pn stays green"; fi
}
setup_loader() { printf 'nvidia_load="YES"\n' >> "$T/basetree/boot/loader.conf"; }
setup_pkg() { mkdir -p "$T/basetree/boot/modules"; : > "$T/basetree/boot/modules/nvidia-uvm.ko"; }
plant_refused loader-guard-off 24 setup_loader 's/boot\/loader.conf boot\/loader.conf.local; do/boot\/nothing; do/'
setup_contline() { mkdir -p "$T/basetree/usr/local/etc/rc.conf.d"; cp "$T/contline" "$T/basetree/usr/local/etc/rc.conf.d/kld"; }
setup_usrlocal() { mkdir -p "$T/basetree/usr/local/etc/rc.conf.d"; printf 'kld_list="nvidia"\n' > "$T/basetree/usr/local/etc/rc.conf.d/kld"; }
plant_refused usr-local-scan-off 27 setup_usrlocal 's/for _d in etc\/rc.conf.d usr\/local\/etc\/rc.conf.d boot\/loader.conf.d; do/for _d in etc\/rc.conf.d boot\/loader.conf.d; do/'
plant_refused continuation-join-off 34 setup_contline 's/sed -e :a -e .*ta. "\$1" 2> \/dev\/null/cat "$1" 2> \/dev\/null/'
plant_refused packaged-module-guard-off 25 setup_pkg 's/for _d in boot\/modules boot\/kernel; do/for _d in boot\/nothing; do/'

# ---- the rc one-shot on fake commands
mkdir -p "$T/rc/bin" "$T/rc/kit/repo/tests/ioctl" "$T/rc/mods" "$T/rc/work" "$T/rc/copyout"
cp "$STAGE/nvidia-v5-phase0.ko" "$T/rc/mods/nvidia.ko"; cp "$STAGE/nvidia-modeset-v5.ko" "$T/rc/mods/nvidia-modeset.ko"; cp "$STAGE/nvidia-uvm-v5.ko" "$T/rc/mods/nvidia-uvm.ko"
cp "$T/list.save" "$T/rc/kit/SHA256-v5.txt"
cp "$KIT/summarize.sh" "$T/rc/kit/summarize.sh"
cp "$REPO/tests/ioctl/uvm_owner_race.c" "$T/rc/kit/repo/tests/ioctl/"
cat > "$T/rc/kit/repo/tests/ioctl/run_owner_boot.sh" <<'EOF'
#!/bin/sh
# a fake run_owner_boot.sh: a result directory with the verdict named by FAKE_VERDICT
D=$GB10_WORK/logs/uvm-2373-boot/20261122T000000Z
mkdir -p "$D"
echo "uids $PROBE_UID_A $PROBE_UID_B seconds $1 ko $NVIDIA_KO cuda $CUDA_VECADD" > "$D/args.txt"
case $FAKE_VERDICT in
PASS) echo "ok   no RmValidateClientData in kenv or sysctl (default: validation on); see nvidia-settings.txt" > "$D/result.txt"
      echo '{"schema":"uvm-owner-race/1","loop":5,"what":"w","iterations_a":3,"iterations_b":9,"open_failed":0,"failed":false,"vacuous":false,"ok":true}' > "$D/race.jsonl"; echo PASS >> "$D/result.txt"; exit 0 ;;
RE-RUN) echo RE-RUN > "$D/result.txt"; exit 3 ;;
*) echo FAIL > "$D/result.txt"; exit 1 ;;
esac
EOF
cat > "$T/rc/bin/kldload" <<'EOF'
#!/bin/sh
echo "kldload $*" >> "$FAKE_LOG"
case "$*" in *nvidia-uvm*) [ -n "${FAKE_UVM_FAILS:-}" ] && { echo "kldload: cannot load" >&2; exit 1; } ;; esac
exit 0
EOF
cat > "$T/rc/bin/kldstat" <<'EOF'
#!/bin/sh
echo "kldstat $*" >> "$FAKE_LOG"
if [ "$1" = -q ]; then
	case "$3" in linux64elf) exit 0 ;; nvidia) [ -n "${FAKE_PRELOADED:-}" ] && exit 0; exit 1 ;; esac
	exit 1
fi
exit 0
EOF
cat > "$T/rc/bin/sysctl" <<'EOF'
#!/bin/sh
echo "sysctl $*" >> "$FAKE_LOG"
case "$*" in
"-n kern.panic_reboot_wait_time debug.debugger_on_panic") printf '%s\n%s\n' "${FAKE_PANIC_WAIT:-5}" "${FAKE_DEBUGGER:-0}" ;;
"-n kern.module_path") echo "/boot/kernel;/boot/modules" ;;
esac
exit 0
EOF
cat > "$T/rc/bin/pgrep" <<'EOF'
#!/bin/sh
echo "pgrep $*" >> "$FAKE_LOG"
[ -z "${FAKE_NO_DEADMAN:-}" ] || exit 1
exit 0
EOF
for c in dmesg bectl shutdown pkill sync; do
	cat > "$T/rc/bin/$c" <<EOF
#!/bin/sh
echo "$c \$*" >> "\$FAKE_LOG"
exit 0
EOF
done
cat > "$T/rc/bin/testboot-return" <<'EOF'
#!/bin/sh
echo "testboot-return $*" >> "$FAKE_LOG"
exit ${FAKE_RETURN_RC:-0}
EOF
chmod +x "$T/rc/bin/"*
cat > "$T/rc/rc.subr" <<'EOF'
load_rc_config() { :; }
run_rc_command() { nvidia_client_owner_testboot_start; wait; }
EOF
RCS=$KIT/nvidia_client_owner_testboot
runrc() {	# runrc <script> <verdict> [uvm fails]: the script is a copy pointed at fakes
	rm -rf "$T/rc/work" "$T/rc/copyout" "$T/rc/fallback"; mkdir -p "$T/rc/work" "$T/rc/copyout"
	: > "$T/rc/log"
	sed -e "s#/etc/rc.subr#$T/rc/rc.subr#" "$1" > "$T/rc/script.sh"
	FAKE_LOG=$T/rc/log FAKE_VERDICT=$2 FAKE_UVM_FAILS=${3:-} PATH=$T/rc/bin:$PATH \
	    nvidia_client_owner_testboot_kit=$T/rc/kit nvidia_client_owner_testboot_mods=$T/rc/mods \
	    nvidia_client_owner_testboot_work=${RC_WORK:-$T/rc/work} nvidia_client_owner_testboot_copyout=$T/rc/copyout \
	    nvidia_client_owner_testboot_return=$T/rc/bin/testboot-return nvidia_client_owner_testboot_cudarun=/kit/cuda.sh \
	    nvidia_client_owner_testboot_version=/nonexistent nvidia_client_owner_testboot_lockfile=/nonexistent/lock \
	    nvidia_client_owner_testboot_deadman=${RC_DEADMAN:-1} nvidia_client_owner_testboot_fallback=${RC_FALLBACK:-$T/rc/fallback} \
	    sh "$T/rc/script.sh" start > "$T/rc/out" 2>&1
}
runrc "$RCS" PASS
RUN=$(ls -d "$T/rc/work"/2* 2>/dev/null | head -1)
if [ -n "$RUN" ] && tail -1 "$T/rc/log" | grep -q '^testboot-return cloudbsd-rx7600$'; then ok "rc: testboot-return NORMAL_BE is the last command"; else cat "$T/rc/log"; cat "$T/rc/out"; bad "rc: testboot-return is not the last command"; fi
first=$(grep -n '^kldload' "$T/rc/log" | head -1)
case $first in *"kldload linux64") ok "rc: linux64 is loaded first" ;; *) bad "rc: linux64 is not loaded first ($first)" ;; esac
grep '^kldload' "$T/rc/log" | grep -q 'nvidia-drm' && bad "rc: nvidia-drm is loaded" || ok "rc: nvidia-drm is never loaded"
[ "$(grep -c '^kldload .*\(nvidia\.ko\|nvidia-modeset\.ko\|nvidia-uvm\.ko\)$' "$T/rc/log")" = 3 ] && ok "rc: nvidia, nvidia-modeset and nvidia-uvm are loaded by full path" || bad "rc: the three modules are not loaded by full path"
grep -q '^sysctl kern.panic_reboot_wait_time=5 debug.debugger_on_panic=0$' "$T/rc/log" && ok "rc: the panic settings are set" || bad "rc: the panic settings are not set"
grep -q "^sysctl kern.module_path=$T/rc/mods;/boot/kernel;/boot/modules\$" "$T/rc/log" && ok "rc: the staged directory comes first on the module path" || bad "rc: the staged directory is not first on the module path"
grep -q 'uids 60003 60004 seconds 60 ko .*/nvidia.ko cuda /kit/cuda.sh' "$RUN"/work/logs/uvm-2373-boot/*/args.txt 2>/dev/null && ok "rc: run_owner_boot.sh gets the probe uids, 60 seconds, the staged nvidia.ko and the kit CUDA script" || bad "rc: run_owner_boot.sh did not get its arguments"
grep -q '"row":"test-uids","result":"pass","detail":"uids 60003 60004 60001 60002 are unused"' "$RUN/results.jsonl" && ok "rc: the uids checked include the race uids read from uvm_owner_race.c" || bad "rc: the race uids are not the ones of uvm_owner_race.c"
grep -q '"row":"done","result":"info"' "$RUN/results.jsonl" && ok "rc: the done row is written" || bad "rc: no done row"
tail -1 "$RUN/summary.txt" | grep -q '^GO$' && ok "rc: a PASS run is read as GO" || { cat "$RUN/summary.txt"; bad "rc: a PASS run is not GO"; }
[ -f "$T/rc/copyout"/*/summary.txt ] && ok "rc: the run is copied out" || bad "rc: the run is not copied out"
grep -q '^pkill' "$T/rc/log" && bad "rc: the dead-man timer was cancelled although the return was not held" || ok "rc: the dead-man timer stays pending after a return that exited 0"
# a held return (exit 3) cancels the dead-man timer; a failed return (exit 2) leaves it
with FAKE_RETURN_RC=3 runrc "$RCS" PASS
grep -q '^pkill' "$T/rc/log" && ok "rc: a held return (exit 3) cancels the dead-man timer" || bad "rc: a held return left the dead-man timer"
with FAKE_RETURN_RC=2 runrc "$RCS" PASS
grep -q '^pkill' "$T/rc/log" && bad "rc: a failed return cancelled the dead-man timer" || ok "rc: a failed return (exit 2) leaves the dead-man timer pending"
# a module that fails to load: still returns, and NO-GO
runrc "$RCS" PASS yes
tail -1 "$T/rc/log" | grep -q '^testboot-return cloudbsd-rx7600$' && ok "rc: a failed load still ends with testboot-return" || bad "rc: a failed load did not end with testboot-return"
RUN=$(ls -d "$T/rc/work"/2* | head -1)
grep -q '"row":"load-nvidia-uvm","result":"fail"' "$RUN/results.jsonl" && tail -1 "$RUN/summary.txt" | grep -q '^NO-GO$' && ok "rc: a failed load of a staged module is NO-GO" || bad "rc: a failed load is not NO-GO"
# a changed module: refused before any load, and KIT (nothing measured)
printf 'tampered\n' > "$T/rc/mods/nvidia.ko"
runrc "$RCS" PASS
grep -q '^kldload' "$T/rc/log" && bad "rc: a module with a wrong hash was loaded" || ok "rc: a module with a wrong hash is not loaded"
tail -1 "$T/rc/log" | grep -q '^testboot-return cloudbsd-rx7600$' && ok "rc: a hash failure still ends with testboot-return" || bad "rc: a hash failure did not end with testboot-return"
RUN=$(ls -d "$T/rc/work"/2* | head -1)
tail -1 "$RUN/summary.txt" | grep -q '^KIT$' && ok "rc: a hash failure is KIT, not NO-GO" || bad "rc: a hash failure is not KIT"
cp "$STAGE/nvidia-v5-phase0.ko" "$T/rc/mods/nvidia.ko"
# panic settings that do not read back as 5 and 0: nothing is loaded
with FAKE_PANIC_WAIT=15 runrc "$RCS" PASS
grep -q '^kldload' "$T/rc/log" && bad "rc: modules were loaded with wrong panic settings" || ok "rc: wrong panic settings load nothing"
RUN=$(ls -d "$T/rc/work"/2* | head -1)
grep -q '"row":"panic-settings","result":"fail"' "$RUN/results.jsonl" && tail -1 "$RUN/summary.txt" | grep -q '^KIT$' && tail -1 "$T/rc/log" | grep -q '^testboot-return' && ok "rc: wrong panic settings are a KIT result and still return" || bad "rc: wrong panic settings are not handled"
# nvidia already loaded: nothing is loaded
with FAKE_PRELOADED=1 runrc "$RCS" PASS
grep -q '^kldload' "$T/rc/log" && bad "rc: modules were loaded over a preloaded nvidia" || ok "rc: a preloaded nvidia is not loaded over"
# a dead-man timer that is not pending: nothing is loaded, KIT, and the return runs (the newest run directory is read)
newest() { ls -dt "$T/rc/work"/2* | head -1; }
with FAKE_NO_DEADMAN=1 runrc "$RCS" PASS
grep -q '^kldload' "$T/rc/log" && bad "rc: modules were loaded with no dead-man timer pending" || ok "rc: with no dead-man timer pending no module is loaded"
RUN=$(newest)
tail -1 "$T/rc/log" | grep -q '^testboot-return cloudbsd-rx7600$' && ok "rc: with no dead-man timer the return still runs" || bad "rc: with no dead-man timer the return did not run"
grep -q '"row":"deadman","result":"fail"' "$RUN/results.jsonl" && grep -q '"row":"load-skipped-no-deadman","result":"fail"' "$RUN/results.jsonl" && tail -1 "$RUN/summary.txt" | grep -q '^KIT$' && ok "rc: no dead-man timer is a KIT result" || { cat "$RUN/results.jsonl"; bad "rc: no dead-man timer is not a KIT result"; }
grep -q 'load-skipped-no-deadman' "$RUN/summary.txt" && ok "rc: the summary names the skipped load" || bad "rc: the summary does not name the failed row"
# a timer disabled with 0 is not a pending timer
with RC_DEADMAN=0 runrc "$RCS" PASS
grep -q '^kldload' "$T/rc/log" && bad "rc: modules were loaded with the dead-man timer disabled" || ok "rc: a dead-man timer disabled with 0 loads nothing"
RUN=$(newest); tail -1 "$RUN/summary.txt" | grep -q '^KIT$' && ok "rc: a disabled dead-man timer is a KIT result" || bad "rc: a disabled dead-man timer is not KIT"
# a setting that is not a number: nothing is loaded, and the reason names the setting
with RC_DEADMAN=10m runrc "$RCS" PASS
grep -q '^kldload' "$T/rc/log" && bad "rc: modules were loaded with a bad dead-man setting" || ok "rc: a bad dead-man setting loads nothing"
RUN=$(newest); grep -q '"row":"load-skipped-no-deadman".*not a number' "$RUN/results.jsonl" && ok "rc: the reason says the setting is not a number" || bad "rc: the reason does not name the setting"
# the run directory cannot be made: the fallback directory of /var/db is used (no /tmp), results are there, the return still runs
: > "$T/rc/afile"
with RC_WORK=$T/rc/afile/sub runrc "$RCS" PASS
tail -1 "$T/rc/log" | grep -q '^testboot-return cloudbsd-rx7600$' && ok "rc: an unusable work directory still ends with testboot-return" || { cat "$T/rc/log"; bad "rc: an unusable work directory skipped testboot-return"; }
FB=$(ls -d "$T/rc/fallback"/2* 2>/dev/null | head -1)
[ -n "$FB" ] && [ "$(stat -f %Lp "$T/rc/fallback")" = 700 ] && [ "$(stat -f %Lp "$FB")" = 700 ] && [ -f "$FB/results.jsonl" ] && ok "rc: the fallback directory is 0700 and holds the results" || bad "rc: the fallback directory was not used"
grep -v '^[[:space:]]*#' "$RCS" | grep -q '/tmp' && bad "rc: the script uses /tmp" || ok "rc: the script never uses /tmp"
# neither directory can be made: no results, no module is loaded, the return still runs
with RC_WORK=$T/rc/afile/sub with RC_FALLBACK=$T/rc/afile/fb runrc "$RCS" PASS
tail -1 "$T/rc/log" | grep -q '^testboot-return cloudbsd-rx7600$' && ok "rc: with no run directory at all the return still runs" || { cat "$T/rc/log"; bad "rc: with no run directory the return did not run"; }
grep -q '^kldload' "$T/rc/log" && bad "rc: modules were loaded with no run directory" || ok "rc: with no run directory no module is loaded"
[ ! -e "$T/rc/afile/fb" ] && ok "rc: no results are written when no directory can be made" || bad "rc: results were written"
# plants on the rc script
plant_rc() {	# plant_rc <name> <sed> <check: function name>
	sed -e "$2" "$RCS" > "$T/rc-plant-$1"
	if cmp -s "$RCS" "$T/rc-plant-$1"; then bad "plant $1 changed nothing"; return; fi
	runrc "$T/rc-plant-$1" PASS
	if "$3"; then bad "plant $1 stays green"; else ok "plant $1 is red"; fi
}
last_is_return() { tail -1 "$T/rc/log" | grep -q '^testboot-return cloudbsd-rx7600$'; }
drm_never() { ! grep '^kldload' "$T/rc/log" | grep -q 'nvidia-drm'; }
linux_first() { grep '^kldload' "$T/rc/log" | head -1 | grep -q 'kldload linux64$'; }
plant_rc return-dropped 's/^\t"\$nvidia_client_owner_testboot_return" "\$NORMAL" .*$/\t:/' last_is_return
plant_rc something-after-return 's/^\techo "== testboot-return exit \$_trc"$/\techo "== testboot-return exit $_trc"; echo after >> "$FAKE_LOG"/' last_is_return
plant_rc drm-loaded 's/for _k in nvidia \$(/for _k in nvidia-drm nvidia $(/' drm_never
plant_rc linux64-not-first 's/kldload linux64 > /kldload nvidia > /' linux_first
sed -e 's/^\tif \[ "\$DM_OK" != 1 \]; then ok=0; .*$/\t:/' "$RCS" > "$T/rc-plant-deadman-not-gating"
if cmp -s "$RCS" "$T/rc-plant-deadman-not-gating"; then bad "plant deadman-not-gating changed nothing"; else
	with FAKE_NO_DEADMAN=1 runrc "$T/rc-plant-deadman-not-gating" PASS
	if grep -q '^kldload' "$T/rc/log"; then ok "plant deadman-not-gating is red (modules load with no dead-man timer)"; else bad "plant deadman-not-gating stays green"; fi
fi
sed -e 's/^\tif check_hashes .*$/\tif true; then :; else ok=0; fi/' "$RCS" > "$T/rc-plant-hash-unchecked"
if cmp -s "$RCS" "$T/rc-plant-hash-unchecked"; then bad "plant hash-unchecked changed nothing"; else
	printf 'tampered\n' > "$T/rc/mods/nvidia.ko"; runrc "$T/rc-plant-hash-unchecked" PASS
	if grep -q '^kldload' "$T/rc/log"; then ok "plant hash-unchecked is red (the changed module is loaded)"; else bad "plant hash-unchecked stays green"; fi
	cp "$STAGE/nvidia-v5-phase0.ko" "$T/rc/mods/nvidia.ko"
fi
sed -e 's/if \[ "\$_ps" = "5 0 " \]; then/if true; then/' "$RCS" > "$T/rc-plant-panic-unchecked"
if cmp -s "$RCS" "$T/rc-plant-panic-unchecked"; then bad "plant panic-unchecked changed nothing"; else
	with FAKE_PANIC_WAIT=15 runrc "$T/rc-plant-panic-unchecked" PASS
	if grep -q '^kldload' "$T/rc/log"; then ok "plant panic-unchecked is red (modules load with wrong settings)"; else bad "plant panic-unchecked stays green"; fi
fi
sed -e 's/^\t\[ "\$_trc" != 3 \] || deadman_cancel$/\tdeadman_cancel/' "$RCS" > "$T/rc-plant-deadman-always"
if cmp -s "$RCS" "$T/rc-plant-deadman-always"; then bad "plant deadman-always changed nothing"; else
	with FAKE_RETURN_RC=2 runrc "$T/rc-plant-deadman-always" PASS
	if grep -q '^pkill' "$T/rc/log"; then ok "plant deadman-always is red (a failed return loses the dead-man timer)"; else bad "plant deadman-always stays green"; fi
fi
plant_fb() {	# plant_fb <name> <sed> <expect>: a plant on the run-directory branches
	sed -e "$2" "$RCS" > "$T/rc-plant-$1"
	if cmp -s "$RCS" "$T/rc-plant-$1"; then bad "plant $1 changed nothing"; return; fi
	case $3 in
	fallback) with RC_WORK=$T/rc/afile/sub runrc "$T/rc-plant-$1" PASS; _f=$(ls -d "$T/rc/fallback"/2* 2>/dev/null | head -1)
	    if [ -n "$_f" ] && [ -f "$_f/results.jsonl" ]; then bad "plant $1 stays green"; else ok "plant $1 is red (the fallback directory is not used)"; fi ;;
	return) with RC_WORK=$T/rc/afile/sub with RC_FALLBACK=$T/rc/afile/fb runrc "$T/rc-plant-$1" PASS
	    if last_is_return; then bad "plant $1 stays green"; else ok "plant $1 is red (with no run directory the return is skipped)"; fi ;;
	esac
}
plant_fb fallback-never-used 's/if mkdir -p "\$FALLBACK" 2> \/dev\/null/if false \&\& mkdir -p "$FALLBACK" 2> \/dev\/null/' fallback
plant_fb no-directory-skips-return 's/^\t\t( "\$nvidia_client_owner_testboot_return" "\$NORMAL" ) > \/dev\/null 2>\&1 \&$/\t\t:/' return
# ---- summarize.sh: the reading rule on fixtures
sumfx() {	# sumfx <name> <rows> <verdict-file-lines> <race row or ''>: builds a run directory, prints its path
	d=$T/sum-$1; rm -rf "$d"; mkdir -p "$d/work/logs/uvm-2373-boot/20261122T000000Z"
	printf '%s\n' "$2" > "$d/results.jsonl"
	[ -z "$3" ] || printf '%s\n' "$3" > "$d/work/logs/uvm-2373-boot/20261122T000000Z/result.txt"
	[ -z "$4" ] || printf '%s\n' "$4" > "$d/work/logs/uvm-2373-boot/20261122T000000Z/race.jsonl"
	echo "$d"
}
ROWS_OK='{"row":"hash-nvidia","result":"pass","detail":"x"}
{"row":"owner-boot","result":"info","detail":"x"}
{"row":"done","result":"info","detail":"x"}'
R5B9='{"schema":"uvm-owner-race/1","loop":5,"what":"w","iterations_a":3,"iterations_b":9,"open_failed":0,"failed":false,"vacuous":false,"ok":true}'
R5B0='{"schema":"uvm-owner-race/1","loop":5,"what":"w","iterations_a":3,"iterations_b":0,"open_failed":0,"failed":false,"vacuous":false,"ok":true}'
READ_OK='ok   RmValidateClientData does not turn validation off'
expect() {	# expect <name> <dir> <exit> <last line>
	out=$(sh "$KIT/summarize.sh" "$2" 2>&1); r=$?
	last=$(printf '%s\n' "$out" | tail -1)
	if [ "$r" = "$3" ] && [ "$last" = "$4" ]; then ok "summarize: $1 is $4"; else printf '%s\n' "$out"; bad "summarize: $1 gave exit $r, '$last', wanted $3, '$4'"; fi
}
expect pass-go "$(sumfx pass "$ROWS_OK" "$READ_OK
PASS" "$R5B9")" 0 GO
expect fail-no-go "$(sumfx fail "$ROWS_OK" "FAIL" "$R5B9")" 1 NO-GO
expect rerun-neither "$(sumfx rerun "$ROWS_OK" "RE-RUN" "$R5B9")" 3 NEITHER
expect pass-unread-neither "$(sumfx unread "$ROWS_OK" "PASS" "$R5B9")" 3 NEITHER
expect loop5-vacuous-neither "$(sumfx l5 "$ROWS_OK" "$READ_OK
PASS" "$R5B0")" 3 NEITHER
expect no-race-row-neither "$(sumfx norace "$ROWS_OK" "$READ_OK
PASS" "")" 3 NEITHER
expect no-done-no-go "$(sumfx nodone '{"row":"hash-nvidia","result":"pass","detail":"x"}' "$READ_OK
PASS" "$R5B9")" 1 NO-GO
expect running-no-go "$(sumfx running '{"row":"owner-boot","result":"running","detail":"x"}' "" "")" 1 NO-GO
expect load-fail-no-go "$(sumfx loadfail '{"row":"load-nvidia-uvm","result":"fail","detail":"x"}
{"row":"done","result":"info","detail":"x"}' "" "")" 1 NO-GO
expect hash-fail-kit "$(sumfx hashfail '{"row":"hash-nvidia","result":"fail","detail":"x"}
{"row":"done","result":"info","detail":"x"}' "" "")" 4 KIT
expect deadman-fail-kit "$(sumfx deadman '{"row":"deadman","result":"fail","detail":"x"}
{"row":"done","result":"info","detail":"x"}' "$READ_OK
PASS" "$R5B9")" 4 KIT
expect kit-and-fail-no-go "$(sumfx kitfail '{"row":"deadman","result":"fail","detail":"x"}
{"row":"load-skipped-no-deadman","result":"fail","detail":"x"}
{"row":"done","result":"info","detail":"x"}' "FAIL" "$R5B9")" 1 NO-GO
expect no-modules-kit "$(sumfx nomod '{"row":"load-skipped-no-deadman","result":"fail","detail":"x"}
{"row":"done","result":"info","detail":"x"}' "" "")" 4 KIT
expect no-result-no-go "$(sumfx nores "$ROWS_OK" "" "")" 1 NO-GO
expect garbage-verdict-no-go "$(sumfx garbage "$ROWS_OK" "MAYBE" "$R5B9")" 1 NO-GO
out=$(sh "$KIT/summarize.sh" "$(sumfx rerunmsg "$ROWS_OK" "RE-RUN" "$R5B9")" 2>&1); case $out in *"bectl activate -t"*) ok "summarize: a RE-RUN says how to run again" ;; *) bad "summarize: a RE-RUN does not say how to run again" ;; esac
out=$(sh "$KIT/summarize.sh" "$(sumfx kitfail2 '{"row":"deadman","result":"fail","detail":"x"}
{"row":"load-skipped-no-deadman","result":"fail","detail":"x"}
{"row":"done","result":"info","detail":"x"}' "FAIL" "$R5B9")" 2>&1); case $out in *"deadman load-skipped-no-deadman"*"fix those"*) ok "summarize: FAIL over KIT names the failed precondition rows" ;; *) bad "summarize: FAIL over KIT does not name the rows: $out" ;; esac
out=$(sh "$KIT/summarize.sh" "$(sumfx nomod2 '{"row":"load-skipped-no-deadman","result":"fail","detail":"x"}
{"row":"done","result":"info","detail":"x"}' "" "")" 2>&1); case $out in *"KIT: a precondition of the test failed: load-skipped-no-deadman"*) ok "summarize: KIT names the failed row" ;; *) bad "summarize: KIT does not name the row: $out" ;; esac
# the message for no result does not contradict itself
out=$(sh "$KIT/summarize.sh" "$(sumfx nores2 "$ROWS_OK" "" "")" 2>&1); case $out in *"found 0"*"read rc.log"*) ok "summarize: no result says found 0 and where to read" ;; *) bad "summarize: the no-result message is wrong: $out" ;; esac
sumplant() {	# sumplant <name> <sed> <fixture dir> <exit> <last>
	sed -e "$2" "$KIT/summarize.sh" > "$T/sum-plant-$1.sh"
	if cmp -s "$KIT/summarize.sh" "$T/sum-plant-$1.sh"; then bad "plant $1 changed nothing"; return; fi
	out=$(sh "$T/sum-plant-$1.sh" "$3" 2>&1); r=$?
	last=$(printf '%s\n' "$out" | tail -1)
	if [ "$r" = "$4" ] && [ "$last" = "$5" ]; then bad "plant $1 stays green"; else ok "plant $1 is red"; fi
}
sumplant rerun-as-fail 's/^\techo NEITHER$/\techo NO-GO/' "$(sumfx p1 "$ROWS_OK" "RE-RUN" "$R5B9")" 3 NEITHER
sumplant fail-ignored '/^FAIL)$/,/exit 1 ;;/s/exit 1/exit 0/' "$(sumfx p2 "$ROWS_OK" "FAIL" "$R5B9")" 1 NO-GO
sumplant read-not-required 's/^if ! grep -Eq .*$/if false; then/' "$(sumfx p3 "$ROWS_OK" "PASS" "$R5B9")" 3 NEITHER
sumplant loop5-not-required 's/^if \[ -z "\$ib" \] || \[ "\$ib" = 0 \]; then$/if false; then/' "$(sumfx p4 "$ROWS_OK" "$READ_OK
PASS" "$R5B0")" 3 NEITHER
sumplant done-not-required 's/!("done" in seen) || last\["done"\] != "info"/0/' "$(sumfx p5 '{"row":"hash-nvidia","result":"pass","detail":"x"}' "$READ_OK
PASS" "$R5B9")" 1 NO-GO
sumplant kit-as-no-go 's/^\techo KIT$/\techo NO-GO/' "$(sumfx p6 '{"row":"hash-nvidia","result":"fail","detail":"x"}
{"row":"done","result":"info","detail":"x"}' "" "")" 4 KIT
sumplant load-as-kit 's/|load\$)/|load$|load-nvidia)/' "$(sumfx p7 '{"row":"load-nvidia","result":"fail","detail":"x"}
{"row":"done","result":"info","detail":"x"}' "" "")" 1 NO-GO

sumplant kit-hides-fail 's/^if \[ "\$kit" != 0 \] && \[ "\$verdict" = FAIL \]; then$/if false; then/' "$(sumfx p8 '{"row":"deadman","result":"fail","detail":"x"}
{"row":"done","result":"info","detail":"x"}' "FAIL" "$R5B9")" 1 NO-GO

# ---- docs: the kit says what it does
for w in 'nvidia-client-owner-test-' 'modules.nvco-test' 'testboot-return' 'panic_reboot_wait_time' 'debugger_on_panic' 'nvidia-drm' 'linux64'; do
	grep -q -- "$w" "$KIT/make-be.sh" "$KIT/nvidia_client_owner_testboot" || bad "the kit does not mention $w"
done
sh "$KIT/make-be.sh" --help > "$T/help.out" 2>&1; [ "$?" = 2 ] && grep -q 'Usage: make-be.sh' "$T/help.out" && ok "make-be.sh --help prints the usage" || bad "make-be.sh --help"

if [ "$rc" = 0 ]; then echo "testboot_kit_test.sh: all checks passed"; else echo "testboot_kit_test.sh: FAILED"; fi
exit $rc
