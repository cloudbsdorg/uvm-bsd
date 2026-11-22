#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# makefile_contract_test.sh - the Makefile of this repository follows the project Makefile contract (purpose
# library: help first, purpose, preflight, build, test) and preflight.sh behaves: the targets run under make (and
# gmake when present), preflight names what is missing, refuses globs and option-like names, and SYSDIR still comes from
# the environment.  Plants (deliberately broken copies) must turn the matching check red.
#
# Run: sh tests/makefile_contract_test.sh

set -u
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
LC_ALL=C
export PATH LC_ALL
TESTSDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$TESTSDIR/.." && pwd)
WORK=${GB10_WORK:-$HOME/work/gb10}
mkdir -p "$WORK/tmp"
T=$(mktemp -d "$WORK/tmp/makefile-contract.XXXXXXXX") && [ -d "$T" ] || { echo "makefile_contract_test.sh: cannot make a scratch directory under $WORK/tmp" >&2; exit 2; }
trap 'rm -rf "$T"' EXIT HUP INT QUIT TERM
rc=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; rc=1; }

cp "$REPO/Makefile" "$REPO/preflight.sh" "$T/"
cd "$T" || exit 2

# help is the first target and the first line names the purpose
[ "$(make 2>&1 | head -1)" = "purpose: library" ] && ok "a bare make prints help, purpose first" || bad "a bare make does not print 'purpose: library' first"
[ "$(make help 2>&1 | head -1)" = "purpose: library" ] && ok "make help: purpose first" || bad "make help: the first line is not the purpose"
[ "$(make purpose 2>&1)" = library ] && ok "make purpose prints the value" || bad "make purpose"
grep -q '^REPO_PURPOSE = library$' Makefile && ok "REPO_PURPOSE is a plain assignment" || bad "REPO_PURPOSE is not declared as REPO_PURPOSE = library"
for t in help purpose preflight build test; do
	make -n "$t" > /dev/null 2>&1 && ok "make -n $t" || bad "make -n $t fails"
done
if ! command -v gmake > /dev/null 2>&1; then echo "skip gmake is not installed: the GNU make checks did not run"; else
	for t in help purpose preflight build test; do
		gmake -r -n "$t" > /dev/null 2>&1 && ok "gmake -r -n $t" || bad "gmake -r -n $t fails"
	done
fi

# preflight
make preflight > "$T/pf.out" 2>&1 && grep -q '^preflight: all [0-9]* command(s) present$' "$T/pf.out" && ok "make preflight passes with the tools present" || { cat "$T/pf.out"; bad "make preflight"; }
make preflight JSON=1 > "$T/pf.json" 2>&1 && grep -q '"purpose": "library"' "$T/pf.json" && grep -q '"schema_version"' "$T/pf.json" && ok "make preflight JSON=1 writes the contract with the purpose" || { cat "$T/pf.json"; bad "make preflight JSON=1"; }
sh preflight.sh --purpose library no-such-tool-xyz > "$T/pf.miss" 2>&1; r=$?
[ "$r" = 1 ] && grep -q 'no-such-tool-xyz' "$T/pf.miss" && ok "preflight exits 1 and names a missing tool" || bad "preflight on a missing tool exited $r"
sh preflight.sh -- -x > /dev/null 2>&1; [ "$?" = 2 ] && ok "preflight refuses a command name that starts with a dash" || bad "preflight accepted a dash name"
mkdir -p "$T/g" && : > "$T/g/make" && ( cd "$T/g" && sh ../preflight.sh --purpose library 'ma*' > "$T/pf.glob" 2>&1 ); [ "$?" = 2 ] && ok "preflight does not glob a command name" || { cat "$T/pf.glob"; bad "preflight globbed 'ma*'"; }

# SYSDIR still comes from the environment, and make build runs in sys/
SYSDIR=/from/env make -n build 2>&1 | grep -q 'SYSDIR="/from/env"' && ok "SYSDIR from the environment reaches the build" || bad "SYSDIR from the environment is ignored"
make -n build SYSDIR=/from/cmd 2>&1 | grep -q 'SYSDIR="/from/cmd"' && ok "SYSDIR on the command line wins" || bad "SYSDIR on the command line is ignored"
make -n build 2>&1 | grep -q '^cd sys && ' && ok "build runs in sys/" || bad "build does not run in sys/"

# plants
plant() {	# plant <name> <file> <sed> <check function>
	sed -e "$3" "$2" > "$T/plant-$1"
	if cmp -s "$2" "$T/plant-$1"; then bad "plant $1 changed nothing"; return; fi
	if "$4" "$T/plant-$1"; then bad "plant $1 stays green"; else ok "plant $1 is red"; fi
}
sysdir_from_env() { mkdir -p "$T/pl"; cp "$1" "$T/pl/Makefile"; ( cd "$T/pl" && SYSDIR=/from/env make -n build 2>&1 | grep -q 'SYSDIR="/from/env"' ); }
plant sysdir-not-overridable "$T/Makefile" 's|^SYSDIR?=.*$|SYSDIR = /usr/src/sys|' sysdir_from_env
no_glob() { ( cd "$T/g" && sh "$1" --purpose library 'ma*' > /dev/null 2>&1; [ "$?" = 2 ] ); }
plant preflight-globs "$T/preflight.sh" 's/^set -f$/:/' no_glob
no_dash() { sh "$1" -- -x > /dev/null 2>&1; [ "$?" = 2 ]; }
plant preflight-dash-name "$T/preflight.sh" 's/^	-\*|\*\[!A-Za-z0-9_.+-\]\*|"")$/	*[!A-Za-z0-9_.+-]*|"")/' no_dash
help_first() { mkdir -p "$T/pl2"; cp "$1" "$T/pl2/Makefile"; [ "$( cd "$T/pl2" && make 2>&1 | head -1 )" = "purpose: library" ]; }
plant help-not-first "$T/Makefile" 's/^help:$/zz-first:\
	@echo not-help\
help:/' help_first

if [ "$rc" = 0 ]; then echo "makefile_contract_test.sh: all checks passed"; else echo "makefile_contract_test.sh: FAILED"; fi
exit $rc
