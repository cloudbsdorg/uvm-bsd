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
