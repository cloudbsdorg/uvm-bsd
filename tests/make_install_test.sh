#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# make_install_test.sh - exercise make install/deinstall as a normal user,
# into test-owned DESTDIR trees only.  No host module or system path is touched.

set -u
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin
LC_ALL=C
export PATH LC_ALL
# Planted modules are test seams too: enable the common gate for this suite.
UVM_TEST_HOOKS=1
export UVM_TEST_HOOKS

TESTSDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$TESTSDIR/.." && pwd)
T=$(mktemp -d "$TESTSDIR/make-install.XXXXXXXX") || {
	echo "make_install_test.sh: cannot create scratch in $TESTSDIR" >&2
	exit 2
}
trap 'rm -rf -- "$T"' EXIT HUP INT QUIT TERM
rc=0

ok() { echo "ok   $*"; }
bad() {
	echo "FAIL $*"
	rc=1
}
show() {
	[ ! -f "$1" ] || sed -n '1,120p' "$1"
}

fake=$T/fake.ko
printf '%s\n' 'planted nvidia-uvm.ko test payload' > "$fake"

# The privileged entry's DESTDIR ownership check is testable without root.
owner_dest=$T/user-owned
mkdir -- "$owner_dest"
UVM_TEST_HOOKS=1 sh "$REPO/scripts/make.sh" _check-dest-owner root "$owner_dest" > "$T/owner-root.out" 2>&1
r=$?
if [ "$r" -ne 0 ] && grep -qF 'not owned by root' "$T/owner-root.out"; then
	ok "root mode refuses a user-owned DESTDIR"
else
	bad "root mode accepted or mishandled a user-owned DESTDIR (exit $r)"
	show "$T/owner-root.out"
fi
UVM_TEST_HOOKS=1 sh "$REPO/scripts/make.sh" _check-dest-owner user "$owner_dest" > "$T/owner-user.out" 2>&1
r=$?
if [ "$r" -eq 0 ]; then
	ok "user mode does not require a root-owned DESTDIR"
else
	bad "user mode rejected a user-owned DESTDIR (exit $r)"
	show "$T/owner-user.out"
fi

# In root mode every DESTDIR ancestor matters, not just the components below
# it.  The hidden check accepts this test's uid in place of root so the walk
# reaches the symlink without a privileged install.
ancestor_target=$T/ancestor-target
ancestor_link=$T/ancestor-link
mkdir -p -- "$ancestor_target/dest"
ln -s -- "$ancestor_target" "$ancestor_link"
UVM_TEST_HOOKS=1 sh "$REPO/scripts/make.sh" _check-destdir-ancestors root \
    "$ancestor_link/dest" > "$T/ancestor-root.out" 2>&1
r=$?
if [ "$r" -ne 0 ] && grep -qF "refusing DESTDIR component $ancestor_link" "$T/ancestor-root.out"; then
	ok "root mode refuses and names a symlinked DESTDIR ancestor"
else
	bad "root mode accepted or did not name a symlinked DESTDIR ancestor (exit $r)"
	show "$T/ancestor-root.out"
fi

# Root also refuses a group- or other-writable ancestor unless it has the
# /tmp-style protection of a sticky bit and trusted ownership.  The hidden
# entry's TEST_UID makes this unprivileged fixture stand in for root ownership.
for writable_mode in 775 777; do
	writable_target=$T/writable-$writable_mode
	mkdir -p -- "$writable_target/dest"
	chmod "$writable_mode" "$writable_target"
	UVM_TEST_HOOKS=1 sh "$REPO/scripts/make.sh" _check-destdir-ancestors root \
	    "$writable_target/dest" > "$T/writable-$writable_mode.out" 2>&1
	r=$?
	if [ "$r" -ne 0 ] && grep -qF 'writable by group or other' "$T/writable-$writable_mode.out"; then
		ok "root mode refuses a mode $writable_mode DESTDIR ancestor"
	else
		bad "root mode accepted a mode $writable_mode DESTDIR ancestor (exit $r)"
		show "$T/writable-$writable_mode.out"
	fi
done
sticky_target=$T/writable-sticky
mkdir -p -- "$sticky_target/dest"
chmod 1777 "$sticky_target"
UVM_TEST_HOOKS=1 sh "$REPO/scripts/make.sh" _check-destdir-ancestors root \
    "$sticky_target/dest" > "$T/writable-sticky.out" 2>&1
r=$?
if [ "$r" -eq 0 ]; then
	ok "root mode permits a trusted sticky DESTDIR ancestor"
else
	bad "root mode rejected a trusted sticky DESTDIR ancestor (exit $r)"
	show "$T/writable-sticky.out"
fi

# A user's staging root may itself sit below a symlink (FreeBSD's /home is the
# common example).  Only components at and below DESTDIR belong to this walk.
user_link_dest=$ancestor_link/dest
(cd "$user_link_dest" && find . | sort) > "$T/user-link.before"
(
	cd "$REPO" || exit 2
	UVM_TEST_HOOKS=1 make install YES=1 DESTDIR="$user_link_dest" MK_KO="$fake"
) > "$T/user-link-install.out" 2>&1
r=$?
if [ "$r" -eq 0 ]; then
	(
		cd "$REPO" || exit 2
		make deinstall YES=1 DESTDIR="$user_link_dest"
	) > "$T/user-link-deinstall.out" 2>&1
	r=$?
fi
(cd "$user_link_dest" && find . | sort) > "$T/user-link.after"
if [ "$r" -eq 0 ] && cmp -s "$T/user-link.before" "$T/user-link.after"; then
	ok "user mode installs and deinstalls below a symlinked DESTDIR ancestor"
else
	bad "user mode mishandled a symlinked DESTDIR ancestor (exit $r)"
	show "$T/user-link-install.out"
	show "$T/user-link-deinstall.out"
	diff -u "$T/user-link.before" "$T/user-link.after"
fi

# The common hook gate must reject effective uid 0 even when UVM_TEST_HOOKS=1.
# A scratch engine's id command simulates that uid without running as root.
hook_root_bin=$T/hook-root-bin
hook_root_dest=$T/hook-root-dest
hook_root_engine=$T/make-hook-root.sh
mkdir -p -- "$hook_root_bin" "$hook_root_dest"
cat > "$hook_root_bin/id" <<'EOF'
#!/bin/sh
if [ "${1:-}" = -u ]; then
	printf '%s\n' 0
else
	exec /usr/bin/id "$@"
fi
EOF
chmod 555 "$hook_root_bin/id"
sed -e "s|^PATH=.*|PATH=$hook_root_bin:/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin|" \
    -e "s|^repo=.*|repo=$REPO|" "$REPO/scripts/make.sh" > "$hook_root_engine"
for hook_entry in _check-dest-owner _check-destdir-ancestors; do
	UVM_TEST_HOOKS=1 sh "$hook_root_engine" "$hook_entry" user "$hook_root_dest" \
	    > "$T/hook-root-$hook_entry.out" 2>&1
	r=$?
	if [ "$r" -ne 0 ] && grep -qF 'test hooks are disabled' "$T/hook-root-$hook_entry.out"; then
		ok "$hook_entry rejects simulated effective uid zero"
	else
		bad "$hook_entry honoured a test seam at simulated effective uid zero (exit $r)"
		show "$T/hook-root-$hook_entry.out"
	fi
done

# The command seam itself is exercised, not merely source-matched.  The real
# install-stage function must call the default kldxref from PATH and must never
# execute the environment-supplied command when the effective uid is zero.
cat > "$hook_root_bin/kldxref" <<'EOF'
#!/bin/sh
printf '%s\n' "$1" > "$KLDXREF_DEFAULT_LOG"
EOF
cat > "$T/kldxref-forbidden" <<'EOF'
#!/bin/sh
printf '%s\n' executed > "$KLDXREF_FORBIDDEN_LOG"
EOF
chmod 555 "$hook_root_bin/kldxref" "$T/kldxref-forbidden"
KLDXREF_DEFAULT_LOG="$T/kldxref-default.log" \
    KLDXREF_FORBIDDEN_LOG="$T/kldxref-forbidden.log" \
    UVM_TEST_KLDXREF="$T/kldxref-forbidden" \
    sh "$hook_root_engine" _install-stage "$hook_root_dest" /usr/local user "$fake" "$fake" \
    > "$T/hook-root-kldxref.out" 2>&1
r=$?
if [ "$r" -eq 0 ] &&
    [ "$(sed -n '1p' "$T/kldxref-default.log" 2>/dev/null)" = "$hook_root_dest/boot/modules" ] &&
    [ ! -e "$T/kldxref-forbidden.log" ]; then
	ok "effective uid zero ignores UVM_TEST_KLDXREF and uses default kldxref"
else
	bad "effective uid zero did not close the kldxref command seam (exit $r)"
	show "$T/hook-root-kldxref.out"
	show "$T/kldxref-default.log"
	show "$T/kldxref-forbidden.log"
fi

# The header names every gated seam so none is an undocumented back door.
hook_header=$(sed -n '1,15p' "$REPO/scripts/make.sh")
header_ok=1
for seam in UVM_TEST_HOOKS=1 MK_KO UVM_TEST_KLDXREF _check-dest-owner/_check-destdir-ancestors; do
	printf '%s\n' "$hook_header" | grep -qF "$seam" || header_ok=0
done
if [ "$header_ok" = 1 ]; then
	ok "the script header documents every gated test seam"
else
	bad "the script header does not document every gated test seam"
fi

# Internal entries accept exactly user and root modes.  Invalid input is a
# usage error, never an alias for either privilege path.
mode_dest=$T/mode-validation
mkdir -- "$mode_dest"
for entry in _install-stage _deinstall; do
	if [ "$entry" = _install-stage ]; then
		sh "$REPO/scripts/make.sh" _install-stage "$mode_dest" /usr/local bogus \
		    "$fake" "$fake" > "$T/mode-$entry.out" 2>&1
	else
		sh "$REPO/scripts/make.sh" _deinstall "$mode_dest" /usr/local bogus \
		    > "$T/mode-$entry.out" 2>&1
	fi
	r=$?
	if [ "$r" -eq 2 ]; then
		ok "$entry rejects an invalid mode with exit 2"
	else
		bad "$entry accepted or mishandled an invalid mode (exit $r)"
		show "$T/mode-$entry.out"
	fi
done

# PREFIX is a manifest field and a path fragment, so a backslash is not data:
# every public and internal entry must reject it as a usage error.
bad_prefix='/usr/local\bad'
for entry in install deinstall _install-stage _deinstall; do
	prefix_reject=$T/prefix-reject-$entry
	mkdir -- "$prefix_reject"
	case $entry in
	install|deinstall)
		MK_YES=1 MK_DESTDIR="$prefix_reject" MK_PREFIX="$bad_prefix" MK_KO="$fake" \
		    sh "$REPO/scripts/make.sh" "$entry" > "$T/prefix-$entry.out" 2>&1
		;;
	_install-stage)
		sh "$REPO/scripts/make.sh" _install-stage "$prefix_reject" "$bad_prefix" user \
		    "$fake" "$fake" > "$T/prefix-$entry.out" 2>&1
		;;
	_deinstall)
		sh "$REPO/scripts/make.sh" _deinstall "$prefix_reject" "$bad_prefix" user \
		    > "$T/prefix-$entry.out" 2>&1
		;;
	esac
	r=$?
	if [ "$r" -eq 2 ] && grep -qF 'PREFIX must be an absolute path' "$T/prefix-$entry.out"; then
		ok "$entry rejects a backslash in PREFIX with exit 2"
	else
		bad "$entry accepted or mishandled a backslash in PREFIX (exit $r)"
		show "$T/prefix-$entry.out"
	fi
done

# No YES and no terminal: refuse before creating even the DESTDIR.
refuse=$T/refuse
(
	cd "$REPO" || exit 2
	make install DESTDIR="$refuse" MK_KO="$fake" < /dev/null
) > "$T/refuse.out" 2>&1
r=$?
if [ "$r" -ne 0 ] && grep -qF 'no terminal to ask on' "$T/refuse.out" && [ ! -e "$refuse" ]; then
	ok "install without YES or a terminal refuses and writes nothing"
else
	bad "install without YES or a terminal was not refused cleanly (exit $r)"
	show "$T/refuse.out"
fi

# Without MK_KO, an existing module is stale until the build has run.  A
# scratch engine differs only in its repository root, so the stub build plants
# its module in the fixture rather than the real source tree.
build_repo=$T/build-repo
build_dest=$T/build-dest
build_stub=$T/build-stub
build_engine=$T/make-build.sh
mkdir -p -- "$build_repo/sys" "$build_repo/man" "$build_dest"
printf '%s\n' 'stale module' > "$build_repo/sys/nvidia-uvm.ko"
printf '%s\n' 'manual fixture' > "$build_repo/man/nvidia-uvm.4"
cat > "$build_stub" <<'EOF'
#!/bin/sh
printf '%s\n' 'build ran' >> "$BUILD_LOG"
printf '%s\n' 'rebuilt module' > "$BUILD_MODULE"
EOF
chmod 555 "$build_stub"
sed "s|^repo=.*|repo=$build_repo|" "$REPO/scripts/make.sh" > "$build_engine"
BUILD_LOG="$T/build.log" BUILD_MODULE="$build_repo/sys/nvidia-uvm.ko" \
    MAKE="$build_stub" MK_YES=1 MK_DESTDIR="$build_dest" \
    sh "$build_engine" install > "$T/build-install.out" 2>&1
r=$?
if [ "$r" -eq 0 ] && grep -qxF 'build ran' "$T/build.log" 2>/dev/null &&
    [ "$(sed -n '1p' "$build_dest/boot/modules/nvidia-uvm.ko")" = 'rebuilt module' ]; then
	ok "install runs the module build even when a stale module exists"
else
	bad "install trusted a stale module without running the build (exit $r)"
	show "$T/build-install.out"
	show "$T/build.log"
fi

# MK_KO is a test seam, not a production build bypass.  Without the common
# hook gate, the normal build runs and its output wins over the planted file.
build_no_hook_dest=$T/build-no-hook-dest
mkdir -- "$build_no_hook_dest"
BUILD_LOG="$T/build-no-hook.log" BUILD_MODULE="$build_repo/sys/nvidia-uvm.ko" \
    MAKE="$build_stub" UVM_TEST_HOOKS=0 MK_KO="$fake" MK_YES=1 MK_DESTDIR="$build_no_hook_dest" \
    sh "$build_engine" install > "$T/build-no-hook.out" 2>&1
r=$?
if [ "$r" -eq 0 ] && grep -qxF 'build ran' "$T/build-no-hook.log" 2>/dev/null &&
    [ "$(sed -n '1p' "$build_no_hook_dest/boot/modules/nvidia-uvm.ko")" = 'rebuilt module' ]; then
	ok "MK_KO without UVM_TEST_HOOKS=1 uses the normal build"
else
	bad "MK_KO without UVM_TEST_HOOKS=1 bypassed the normal build (exit $r)"
	show "$T/build-no-hook.out"
	show "$T/build-no-hook.log"
fi

# With the common gate enabled, the same planted module is honoured and the
# build stub must not run.  This distinguishes the test path from production.
build_hook_dest=$T/build-hook-dest
mkdir -- "$build_hook_dest"
BUILD_LOG="$T/build-hook.log" BUILD_MODULE="$build_repo/sys/nvidia-uvm.ko" \
    MAKE="$build_stub" UVM_TEST_HOOKS=1 MK_KO="$fake" MK_YES=1 MK_DESTDIR="$build_hook_dest" \
    sh "$build_engine" install > "$T/build-hook.out" 2>&1
r=$?
if [ "$r" -eq 0 ] && [ ! -e "$T/build-hook.log" ] &&
    [ "$(sed -n '1p' "$build_hook_dest/boot/modules/nvidia-uvm.ko")" = 'planted nvidia-uvm.ko test payload' ]; then
	ok "MK_KO with UVM_TEST_HOOKS=1 uses the planted module"
else
	bad "MK_KO with UVM_TEST_HOOKS=1 did not select the planted module (exit $r)"
	show "$T/build-hook.out"
	show "$T/build-hook.log"
fi

# At simulated effective uid zero, MK_KO must not bypass the build even with
# UVM_TEST_HOOKS=1.  The build stub fails before any privileged install stage.
hook_root_build_dest=$T/hook-root-build-dest
root_build_stub=$T/root-build-stub
mkdir -- "$hook_root_build_dest"
cat > "$root_build_stub" <<'EOF'
#!/bin/sh
printf '%s\n' 'root build attempted' > "$ROOT_BUILD_LOG"
exit 1
EOF
chmod 555 "$root_build_stub"
ROOT_BUILD_LOG="$T/root-build.log" MAKE="$root_build_stub" MK_KO="$fake" \
    MK_YES=1 MK_DESTDIR="$hook_root_build_dest" UVM_TEST_HOOKS=1 \
    sh "$hook_root_engine" install > "$T/root-build.out" 2>&1
r=$?
if [ "$r" -ne 0 ] && grep -qxF 'root build attempted' "$T/root-build.log" 2>/dev/null &&
    [ ! -e "$hook_root_build_dest/boot/modules/nvidia-uvm.ko" ]; then
	ok "effective uid zero ignores MK_KO and attempts the normal build"
else
	bad "effective uid zero honoured MK_KO or skipped the normal build (exit $r)"
	show "$T/root-build.out"
	show "$T/root-build.log"
fi

# A reading-tar failure must not disappear into a pipeline's last status.
# The mode 000 source is the unreadable fixture; the controlled reader also
# supplies a valid archive before exiting 1, ensuring only status propagation
# can reject it.  Other tar invocations, including the extractor, use real tar.
unreadable=$T/unreadable.ko
unreadable_dest=$T/unreadable-dest
unreadable_bin=$T/unreadable-bin
unreadable_engine=$T/make-unreadable.sh
printf '%s\n' 'unreadable module fixture' > "$unreadable"
chmod 000 "$unreadable"
mkdir -p -- "$unreadable_dest" "$unreadable_bin" "$T/unreadable-archive-source"
printf '%s\n' 'archive produced before reader error' > "$T/unreadable-archive-source/unreadable.ko"
/usr/bin/tar -C "$T/unreadable-archive-source" -cf "$T/unreadable.tar" ./unreadable.ko
cat > "$unreadable_bin/tar" <<'EOF'
#!/bin/sh
for arg do
	[ "$arg" = ./unreadable.ko ] || continue
	output=
	previous=
	for arg do
		if [ "$previous" = -cf ]; then
			output=$arg
		fi
		previous=$arg
	done
	if [ "$output" = - ]; then
		/bin/cat "$TAR_ARCHIVE"
	else
		/bin/cp "$TAR_ARCHIVE" "$output"
	fi
	exit 1
done
exec /usr/bin/tar "$@"
EOF
chmod 555 "$unreadable_bin/tar"
sed "s|^PATH=.*|PATH=$unreadable_bin:/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin|" \
    "$REPO/scripts/make.sh" > "$unreadable_engine"
(cd "$unreadable_dest" && find . | sort) > "$T/unreadable.before"
TAR_ARCHIVE="$T/unreadable.tar" sh "$unreadable_engine" \
    _install-stage "$unreadable_dest" /usr/local user "$unreadable" "$fake" \
    > "$T/unreadable.out" 2>&1
r=$?
(cd "$unreadable_dest" && find . | sort) > "$T/unreadable.after"
if [ "$r" -ne 0 ] && cmp -s "$T/unreadable.before" "$T/unreadable.after"; then
	ok "an unreadable source fails the install and writes nothing"
else
	bad "an unreadable source was installed or changed the DESTDIR (exit $r)"
	show "$T/unreadable.out"
	diff -u "$T/unreadable.before" "$T/unreadable.after"
fi
chmod 600 "$unreadable"

# The kldxref command seam substitutes only the command, not the install
# engine.  Its failure must be reported without aborting, and it must observe
# the final manifest with both payloads already in place.
kldfail_dest=$T/kldxref-failure
kldfail_stub=$T/kldxref-fail
mkdir -- "$kldfail_dest"
cat > "$kldfail_stub" <<'EOF'
#!/bin/sh
root=${1%/boot/modules}
manifest=$root/var/db/uvm-bsd/installed.manifest
if [ -f "$root/boot/modules/nvidia-uvm.ko" ] &&
    [ -f "$root/usr/local/share/man/man4/nvidia-uvm.4" ] &&
    [ -f "$manifest" ] &&
    ! awk '$2 == "pending" { found = 1 } END { exit !found }' "$manifest"; then
	printf 'final %s\n' "$1" >> "$KLDXREF_FAIL_LOG"
else
	printf 'not-final %s\n' "$1" >> "$KLDXREF_FAIL_LOG"
fi
exit 1
EOF
chmod 555 "$kldfail_stub"
(
	cd "$REPO" || exit 2
	KLDXREF_FAIL_LOG="$T/kldxref-failure.log" UVM_TEST_HOOKS=1 \
	    UVM_TEST_KLDXREF="$kldfail_stub" \
	    make install YES=1 DESTDIR="$kldfail_dest" MK_KO="$fake"
) > "$T/kldxref-failure.out" 2>&1
r=$?
if [ "$r" -eq 0 ] &&
    grep -qxF "final $kldfail_dest/boot/modules" "$T/kldxref-failure.log" 2>/dev/null &&
    grep -qF "kldxref $kldfail_dest/boot/modules failed" "$T/kldxref-failure.out"; then
	ok "a kldxref failure is nonfatal and runs after the final manifest"
else
	bad "a kldxref failure aborted install or ran before the final manifest (exit $r)"
	show "$T/kldxref-failure.out"
	show "$T/kldxref-failure.log"
fi

# Exercise run_kldxref itself with a failing false command and empty DESTDIR.
# This covers the live-install branch without touching the host's /boot.  The
# preceding fixture proves that the call happens after the manifest is final.
awk '/^run_kldxref\(\) \{$/ { copy = 1 } copy { print } copy && /^}$/ { exit }' \
    "$REPO/scripts/make.sh" | sed 's|_kx=kldxref|_kx=false|' > "$T/live-kldxref-function.sh"
{
	printf '%s\n' 'set -u' 'DESTDIR=' 'test_hooks_enabled() { return 1; }' \
	    'note() { printf "%s\\n" "$*"; }'
	cat "$T/live-kldxref-function.sh"
	printf '%s\n' 'run_kldxref /boot/modules'
} > "$T/live-kldxref-test.sh"
sh "$T/live-kldxref-test.sh" > "$T/live-kldxref.out" 2>&1
r=$?
if [ "$r" -eq 1 ] && grep -qxF \
    'kldxref /boot/modules failed: the module is installed but the loader index is stale; run doas kldxref /boot/modules' \
    "$T/live-kldxref.out"; then
	ok "a live kldxref failure gives the remedy and exits 1"
else
	bad "a live kldxref failure was not fatal with the required remedy (exit $r)"
	show "$T/live-kldxref.out"
fi

# Install controls the modes it creates instead of inheriting a caller's
# restrictive umask.  The pre-existing DESTDIR is normalized for the fixture;
# every directory below it is the install's work.
umask_dest=$T/umask
mkdir -- "$umask_dest"
chmod 755 "$umask_dest"
(
	umask 077
	cd "$REPO" || exit 2
	make install YES=1 DESTDIR="$umask_dest" MK_KO="$fake"
) > "$T/umask.out" 2>&1
r=$?
umask_bad_dirs=$(find "$umask_dest" -type d ! -perm 755 -print)
if [ "$r" -eq 0 ] && [ -z "$umask_bad_dirs" ]; then
	ok "install under umask 077 creates only mode 755 directories"
else
	bad "install under umask 077 created non-755 directories (exit $r)"
	show "$T/umask.out"
	printf '%s\n' "$umask_bad_dirs"
fi

# There must be no source-level window between creating the private directory
# and installing its cleanup traps.  The runtime signal test below then proves
# that those traps remove the directory once copying is in progress.
trap_order=$(sed -n '/^install_stage() {$/,/^}$/p' "$REPO/scripts/make.sh" | awk '
/^[[:space:]]*_w=$/ && !init { init = NR }
/^[[:space:]]*trap .*EXIT/ && !cleanup { cleanup = NR }
/_w=\$\(mktemp -d/ && !create { create = NR }
END {
	if (init && cleanup && create && init < cleanup && cleanup < create)
		print "ordered"
	else
		printf "unordered init=%d cleanup=%d create=%d\n", init, cleanup, create
}')
if [ "$trap_order" = ordered ]; then
	ok "cleanup traps are armed before the private directory is created"
else
	bad "private-directory creation precedes safe cleanup traps ($trap_order)"
fi

# Signal the real install engine while its private tar copy is in progress.
# Only PATH differs in the scratch engine.  The blocking tar is always killed
# after the engine is signalled, including when an assertion fails.
signal_dest=$T/signal-cleanup
signal_bin=$T/signal-bin
signal_engine=$T/make-signal.sh
mkdir -p -- "$signal_dest" "$signal_bin"
cat > "$signal_bin/tar" <<'EOF'
#!/bin/sh
printf '%s\n' "$$" > "$TAR_PID_FILE"
: > "$TAR_STARTED"
exec sleep 30
EOF
chmod 555 "$signal_bin/tar"
sed "s|^PATH=.*|PATH=$signal_bin:/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin|" \
    "$REPO/scripts/make.sh" > "$signal_engine"
TAR_STARTED="$T/signal-tar.started" TAR_PID_FILE="$T/signal-tar.pid" \
    sh "$signal_engine" _install-stage "$signal_dest" /usr/local user "$fake" "$fake" \
    > "$T/signal.out" 2>&1 &
signal_pid=$!
for attempt in 1 2 3 4 5 6 7 8 9 10; do
	[ ! -f "$T/signal-tar.started" ] || break
	kill -0 "$signal_pid" 2>/dev/null || break
	sleep 1
done
if [ -f "$T/signal-tar.started" ]; then
	kill -TERM "$signal_pid" 2>/dev/null
fi
tar_pid=
if [ -f "$T/signal-tar.pid" ]; then
	read -r tar_pid < "$T/signal-tar.pid"
fi
case $tar_pid in
''|*[!0-9]*) ;;
*) kill -TERM "$tar_pid" 2>/dev/null ;;
esac
wait "$signal_pid"
r=$?
signal_left=$(find "$signal_dest" -mindepth 1 -maxdepth 1 -name '.uvm-bsd.install.*' -print)
if [ -f "$T/signal-tar.started" ] && [ "$r" -ne 0 ] && [ -z "$signal_left" ]; then
	ok "a signal during install removes the private directory"
else
	bad "a signal during install left a private directory or did not stop install (exit $r)"
	show "$T/signal.out"
	printf '%s\n' "$signal_left"
fi

# A normal-user DESTDIR install writes exactly the two payloads with their
# modes, ownership and manifest hashes.  kldxref may reject the planted file;
# that is explicitly non-fatal for a DESTDIR.
dest=$T/basic
mkdir -- "$dest"
(cd "$dest" && find . | sort) > "$T/basic.before"
(
	cd "$REPO" || exit 2
	make install YES=1 DESTDIR="$dest" MK_KO="$fake"
) > "$T/install.out" 2>&1
r=$?
manifest=$dest/var/db/uvm-bsd/installed.manifest
ko=$dest/boot/modules/nvidia-uvm.ko
man=$dest/usr/local/share/man/man4/nvidia-uvm.4
ko_sha=
man_sha=
owner=$(id -un):$(id -gn)
[ ! -f "$ko" ] || ko_sha=$(sha256 -q "$ko")
[ ! -f "$man" ] || man_sha=$(sha256 -q "$man")
if [ "$r" -eq 0 ] && [ -f "$ko" ] && [ ! -L "$ko" ] && [ -f "$man" ] && [ ! -L "$man" ] &&
    [ "$(stat -f '%Lp %Su:%Sg' "$ko")" = "555 $owner" ] &&
    [ "$(stat -f '%Lp %Su:%Sg' "$man")" = "444 $owner" ] &&
    [ -f "$manifest" ] && [ ! -L "$manifest" ] &&
    [ "$(awk '$1 == "/boot/modules/nvidia-uvm.ko" && $2 == "555" && $3 == "'"$owner"'" { print $4 }' "$manifest")" = "$ko_sha" ] &&
    [ "$(awk '$1 == "/usr/local/share/man/man4/nvidia-uvm.4" && $2 == "444" && $3 == "'"$owner"'" { print $4 }' "$manifest")" = "$man_sha" ] &&
    ! awk '$2 == "pending" { found = 1 } END { exit !found }' "$manifest"; then
	ok "DESTDIR install writes both files and matching manifest hashes"
else
	bad "DESTDIR install did not produce the two files and a matching manifest (exit $r)"
	show "$T/install.out"
	[ ! -f "$manifest" ] || show "$manifest"
fi

# Environment DESTDIR must stage rather than silently selecting a live install.
# Dry-run first so a regression is red without risking the host's live tree.
env_dest=$T/environment
mkdir -- "$env_dest"
(
	cd "$REPO" || exit 2
	env DESTDIR="$env_dest" make -n install YES=1 MK_KO="$fake"
) > "$T/env-dry.out" 2>&1
if grep -qF "MK_DESTDIR=\"$env_dest\"" "$T/env-dry.out"; then
	(
		cd "$REPO" || exit 2
		env DESTDIR="$env_dest" make install YES=1 MK_KO="$fake"
	) > "$T/env-install.out" 2>&1
	r=$?
	if [ "$r" -eq 0 ] && [ -f "$env_dest/boot/modules/nvidia-uvm.ko" ] &&
	    [ -f "$env_dest/usr/local/share/man/man4/nvidia-uvm.4" ]; then
		ok "environment DESTDIR installs into the scratch tree"
	else
		bad "environment DESTDIR did not install into the scratch tree (exit $r)"
		show "$T/env-install.out"
	fi
else
	bad "environment DESTDIR is discarded before install"
	show "$T/env-dry.out"
fi

# Reinstall is an upgrade of files the manifest still identifies as ours.
(
	cd "$REPO" || exit 2
	make install YES=1 DESTDIR="$dest" MK_KO="$fake"
) > "$T/reinstall.out" 2>&1
r=$?
if [ "$r" -eq 0 ] && [ "$(sha256 -q "$ko")" = "$ko_sha" ] && [ "$(sha256 -q "$man")" = "$man_sha" ]; then
	ok "a second install is allowed"
else
	bad "a second install failed (exit $r)"
	show "$T/reinstall.out"
fi

# PREFIX belongs to the manifest: changing it requires deinstall first, and
# deinstall must use the recorded prefix rather than the command-line value.
prefix_dest=$T/prefix
mkdir -- "$prefix_dest"
(cd "$prefix_dest" && find . | sort) > "$T/prefix.before"
(
	cd "$REPO" || exit 2
	make install YES=1 DESTDIR="$prefix_dest" MK_KO="$fake"
) > "$T/prefix-install.out" 2>&1
r=$?
if [ "$r" -eq 0 ] &&
    [ "$(sed -n '1p' "$prefix_dest/var/db/uvm-bsd/installed.manifest")" = 'prefix /usr/local' ]; then
	(
		cd "$REPO" || exit 2
		make install YES=1 DESTDIR="$prefix_dest" PREFIX=/opt/uvm-other MK_KO="$fake"
	) > "$T/prefix-reinstall.out" 2>&1
	r=$?
	if [ "$r" -ne 0 ] && grep -qF 'deinstall first' "$T/prefix-reinstall.out"; then
		ok "install refuses a different PREFIX while a manifest exists"
	else
		bad "install accepted a different PREFIX with an existing manifest (exit $r)"
		show "$T/prefix-reinstall.out"
	fi
	(
		cd "$REPO" || exit 2
		make deinstall YES=1 DESTDIR="$prefix_dest" PREFIX=/opt/uvm-other
	) > "$T/prefix-deinstall.out" 2>&1
	r=$?
	(cd "$prefix_dest" && find . | sort) > "$T/prefix.after"
	if [ "$r" -eq 0 ] && cmp -s "$T/prefix.before" "$T/prefix.after"; then
		ok "deinstall uses the manifest PREFIX and restores the DESTDIR"
	else
		bad "recorded PREFIX deinstall did not restore the DESTDIR (exit $r)"
		show "$T/prefix-deinstall.out"
		diff -u "$T/prefix.before" "$T/prefix.after"
	fi
else
	bad "could not establish the PREFIX fixture or its manifest first line (exit $r)"
	show "$T/prefix-install.out"
fi

# A pre-narrowing install can contain a recorded PREFIX that a new command
# line would reject.  Deinstall trusts that manifest record as data: it must
# remove the two matching payloads instead of stranding the old install.
legacy_dest=$T/legacy-prefix
legacy_prefix=/opt/uvm@old
legacy_manifest=$legacy_dest/var/db/uvm-bsd/installed.manifest
legacy_ko=$legacy_dest/boot/modules/nvidia-uvm.ko
legacy_man=$legacy_dest$legacy_prefix/share/man/man4/nvidia-uvm.4
mkdir -p -- "$legacy_dest/boot/modules" "$legacy_dest/var/db/uvm-bsd" \
    "$legacy_dest$legacy_prefix/share/man/man4"
printf '%s\n' 'legacy module' > "$legacy_ko"
printf '%s\n' 'legacy manual' > "$legacy_man"
chmod 555 "$legacy_ko"
chmod 444 "$legacy_man"
legacy_owner=$(id -un):$(id -gn)
{
	printf 'prefix %s\n' "$legacy_prefix"
	printf '%s 555 %s %s\n' /boot/modules/nvidia-uvm.ko "$legacy_owner" "$(sha256 -q "$legacy_ko")"
	printf '%s 444 %s %s\n' "$legacy_prefix/share/man/man4/nvidia-uvm.4" "$legacy_owner" "$(sha256 -q "$legacy_man")"
	printf '%s 444 %s -\n' /var/db/uvm-bsd/installed.manifest "$legacy_owner"
} > "$legacy_manifest"
chmod 444 "$legacy_manifest"
sh "$REPO/scripts/make.sh" _deinstall "$legacy_dest" /usr/local user \
    > "$T/legacy-prefix.out" 2>&1
r=$?
if [ "$r" -eq 0 ] && [ ! -e "$legacy_ko" ] && [ ! -e "$legacy_man" ] && [ ! -e "$legacy_manifest" ]; then
	ok "deinstall accepts a recorded PREFIX outside the new character set"
else
	bad "deinstall stranded a recorded legacy PREFIX (exit $r)"
	show "$T/legacy-prefix.out"
fi

# An unlisted file is foreign, even at the module's normal destination.
foreign=$T/foreign
mkdir -p -- "$foreign/boot/modules"
printf '%s\n' 'foreign module: do not replace' > "$foreign/boot/modules/nvidia-uvm.ko"
foreign_sha=$(sha256 -q "$foreign/boot/modules/nvidia-uvm.ko")
(cd "$foreign" && find . | sort) > "$T/foreign.before"
(
	cd "$REPO" || exit 2
	make install YES=1 DESTDIR="$foreign" MK_KO="$fake"
) > "$T/foreign.out" 2>&1
r=$?
(cd "$foreign" && find . | sort) > "$T/foreign.after"
if [ "$r" -ne 0 ] && grep -qF 'make install did not write it' "$T/foreign.out" &&
    [ "$(sha256 -q "$foreign/boot/modules/nvidia-uvm.ko")" = "$foreign_sha" ] &&
    [ ! -e "$foreign/usr/local/share/man/man4/nvidia-uvm.4" ] &&
    cmp -s "$T/foreign.before" "$T/foreign.after"; then
	ok "a refused foreign-module install leaves the DESTDIR exactly as before"
else
	bad "a refused foreign-module install changed the DESTDIR (exit $r)"
	show "$T/foreign.out"
	diff -u "$T/foreign.before" "$T/foreign.after"
fi

# A completed manifest does not own a sibling .new path.  Only a pending
# record authorizes removing the temporary left by an interrupted install.
new_dest=$T/completed-new
mkdir -- "$new_dest"
(
	cd "$REPO" || exit 2
	make install YES=1 DESTDIR="$new_dest" MK_KO="$fake"
) > "$T/new-install.out" 2>&1
r=$?
if [ "$r" -eq 0 ]; then
	printf '%s\n' 'foreign temporary: do not remove' > "$new_dest/boot/modules/nvidia-uvm.ko.new"
	(
		cd "$REPO" || exit 2
		make deinstall YES=1 DESTDIR="$new_dest"
	) > "$T/new-deinstall.out" 2>&1
	r=$?
	if [ "$r" -eq 0 ] &&
	    [ "$(sed -n '1p' "$new_dest/boot/modules/nvidia-uvm.ko.new")" = 'foreign temporary: do not remove' ] &&
	    [ ! -e "$new_dest/boot/modules/nvidia-uvm.ko" ]; then
		ok "deinstall preserves .new without a pending manifest line"
	else
		bad "deinstall removed an unrecorded .new path (exit $r)"
		show "$T/new-deinstall.out"
	fi
else
	bad "could not establish the completed-manifest .new fixture (exit $r)"
	show "$T/new-install.out"
fi

# DESTDIR is not a manifest field: spaces and a literal '*' must reach every
# path operation without word splitting or pathname expansion.
glob_dest="$T/space * stage"
mkdir -- "$glob_dest"
(cd "$glob_dest" && find . | sort) > "$T/glob.before"
(
	cd "$REPO" || exit 2
	make install YES=1 DESTDIR="$glob_dest" MK_KO="$fake"
) > "$T/glob-install.out" 2>&1
r=$?
if [ "$r" -eq 0 ] && [ -f "$glob_dest/boot/modules/nvidia-uvm.ko" ]; then
	(
		cd "$REPO" || exit 2
		make deinstall YES=1 DESTDIR="$glob_dest"
	) > "$T/glob-deinstall.out" 2>&1
	r=$?
	(cd "$glob_dest" && find . | sort) > "$T/glob.after"
	if [ "$r" -eq 0 ] && cmp -s "$T/glob.before" "$T/glob.after"; then
		ok "a DESTDIR containing a space and '*' installs and deinstalls cleanly"
	else
		bad "deinstall failed for a DESTDIR containing a space and '*' (exit $r)"
		show "$T/glob-deinstall.out"
		diff -u "$T/glob.before" "$T/glob.after"
	fi
else
	bad "install failed for a DESTDIR containing a space and '*' (exit $r)"
	show "$T/glob-install.out"
fi

# kldxref must run while boot/modules still exists.  Replace only PATH in a
# scratch copy so a fake kldxref can prove that ordering and create the hints
# file a successful real run would write.  The install itself uses the real
# engine and every destination remains in this test's DESTDIR.
fakebin=$T/kldxref-bin
mkdir -- "$fakebin"
cat > "$fakebin/kldxref" <<'EOF'
#!/bin/sh
if [ ! -d "$1" ]; then
	printf 'missing %s\n' "$1" >> "$KLDXREF_LOG"
	exit 1
fi
printf 'present %s\n' "$1" >> "$KLDXREF_LOG"
printf '%s\n' 'refreshed test hints' > "$1/linker.hints"
EOF
chmod 555 "$fakebin/kldxref"
sed "s|^PATH=.*|PATH=$fakebin:/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin|" \
    "$REPO/scripts/make.sh" > "$T/make-kldxref.sh"

hints_order=$T/hints-order
mkdir -- "$hints_order"
(cd "$hints_order" && find . | sort) > "$T/hints-order.before"
(
	cd "$REPO" || exit 2
	make install YES=1 DESTDIR="$hints_order" MK_KO="$fake"
) > "$T/hints-order-install.out" 2>&1
r=$?
if [ "$r" -eq 0 ]; then
	rm -f -- "$hints_order/boot/modules/linker.hints"
	: > "$T/hints-order.log"
	KLDXREF_LOG="$T/hints-order.log" sh "$T/make-kldxref.sh" \
	    _deinstall "$hints_order" /usr/local user > "$T/hints-order-deinstall.out" 2>&1
	r=$?
	(cd "$hints_order" && find . | sort) > "$T/hints-order.after"
	if [ "$r" -eq 0 ] && grep -qF "present $hints_order/boot/modules" "$T/hints-order.log" &&
	    cmp -s "$T/hints-order.before" "$T/hints-order.after"; then
		ok "deinstall calls kldxref before removing a module directory it created"
	else
		bad "deinstall did not call kldxref before removing its module directory (exit $r)"
		show "$T/hints-order-deinstall.out"
		show "$T/hints-order.log"
		diff -u "$T/hints-order.before" "$T/hints-order.after"
	fi
else
	bad "could not establish the kldxref ordering fixture (install exit $r)"
	show "$T/hints-order-install.out"
fi

hints_created=$T/hints-created
mkdir -- "$hints_created"
(cd "$hints_created" && find . | sort) > "$T/hints-created.before"
(
	cd "$REPO" || exit 2
	make install YES=1 DESTDIR="$hints_created" MK_KO="$fake"
) > "$T/hints-created-install.out" 2>&1
r=$?
if [ "$r" -eq 0 ]; then
	printf '%s\n' 'hints written by install' > "$hints_created/boot/modules/linker.hints"
	: > "$T/hints-created.log"
	KLDXREF_LOG="$T/hints-created.log" sh "$T/make-kldxref.sh" \
	    _deinstall "$hints_created" /usr/local user > "$T/hints-created-deinstall.out" 2>&1
	r=$?
	(cd "$hints_created" && find . | sort) > "$T/hints-created.after"
	if [ "$r" -eq 0 ] && grep -qF "present $hints_created/boot/modules" "$T/hints-created.log" &&
	    cmp -s "$T/hints-created.before" "$T/hints-created.after"; then
		ok "deinstall removes linker.hints from a module directory it created"
	else
		bad "deinstall left linker.hints in a module directory it created (exit $r)"
		show "$T/hints-created-deinstall.out"
		show "$T/hints-created.log"
		diff -u "$T/hints-created.before" "$T/hints-created.after"
	fi
else
	bad "could not establish the created-hints fixture (install exit $r)"
	show "$T/hints-created-install.out"
fi

hints_existing=$T/hints-existing
mkdir -p -- "$hints_existing/boot/modules"
printf '%s\n' 'pre-existing hints' > "$hints_existing/boot/modules/linker.hints"
(
	cd "$REPO" || exit 2
	make install YES=1 DESTDIR="$hints_existing" MK_KO="$fake"
) > "$T/hints-existing-install.out" 2>&1
r=$?
if [ "$r" -eq 0 ]; then
	: > "$T/hints-existing.log"
	KLDXREF_LOG="$T/hints-existing.log" sh "$T/make-kldxref.sh" \
	    _deinstall "$hints_existing" /usr/local user > "$T/hints-existing-deinstall.out" 2>&1
	r=$?
	if [ "$r" -eq 0 ] && grep -qF "present $hints_existing/boot/modules" "$T/hints-existing.log" &&
	    [ "$(sed -n '1p' "$hints_existing/boot/modules/linker.hints")" = 'refreshed test hints' ]; then
		ok "deinstall preserves linker.hints in a pre-existing module directory"
	else
		bad "deinstall mishandled hints in a pre-existing module directory (exit $r)"
		show "$T/hints-existing-deinstall.out"
		show "$T/hints-existing.log"
	fi
else
	bad "could not establish the pre-existing-hints fixture (install exit $r)"
	show "$T/hints-existing-install.out"
fi

# A changed file survives deinstall, stays listed, and makes the exit nonzero.
changed=$T/changed
mkdir -- "$changed"
(
	cd "$REPO" || exit 2
	make install YES=1 DESTDIR="$changed" MK_KO="$fake"
) > "$T/changed-install.out" 2>&1
r=$?
if [ "$r" -eq 0 ]; then
	chmod 644 "$changed/usr/local/share/man/man4/nvidia-uvm.4"
	printf '%s\n' 'locally changed manual' > "$changed/usr/local/share/man/man4/nvidia-uvm.4"
	(
		cd "$REPO" || exit 2
		make deinstall YES=1 DESTDIR="$changed"
	) > "$T/changed-deinstall.out" 2>&1
	r=$?
	if [ "$r" -eq 1 ] && grep -qF 'changed since make install wrote it' "$T/changed-deinstall.out" &&
	    [ "$(sed -n '1p' "$changed/usr/local/share/man/man4/nvidia-uvm.4")" = 'locally changed manual' ] &&
	    [ ! -e "$changed/boot/modules/nvidia-uvm.ko" ] &&
	    awk '$1 == "/usr/local/share/man/man4/nvidia-uvm.4" { found = 1 } END { exit !found }' \
	        "$changed/var/db/uvm-bsd/installed.manifest"; then
		ok "deinstall keeps a changed man page, listed, and exits 1"
	else
		bad "deinstall did not preserve and report the changed man page (exit $r)"
		show "$T/changed-deinstall.out"
	fi
else
	bad "could not establish the changed-file fixture (install exit $r)"
	show "$T/changed-install.out"
fi

# With no changed files, deinstall removes every file and directory it made.
(
	cd "$REPO" || exit 2
	make deinstall YES=1 DESTDIR="$dest"
) > "$T/deinstall.out" 2>&1
r=$?
(cd "$dest" && find . | sort) > "$T/basic.after"
if [ "$r" -eq 0 ] && cmp -s "$T/basic.before" "$T/basic.after"; then
	ok "deinstall leaves the DESTDIR exactly as before"
else
	bad "deinstall did not restore the DESTDIR (exit $r)"
	show "$T/deinstall.out"
	diff -u "$T/basic.before" "$T/basic.after"
fi

if [ "$rc" -eq 0 ]; then
	echo "make_install_test.sh: all checks passed"
else
	echo "make_install_test.sh: FAILED"
fi
exit "$rc"
