#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# Text check. This host has no bmake; this script does not run make.
#
# The port Makefile must, outside any conditional:
#   include ${PORTSDIR}/x11/nvidia-driver/Makefile.version once
#   set DISTVERSION=${NVIDIA_DISTVERSION} once, with operator =
#   .error when that file is missing and when NVIDIA_DISTVERSION is empty
#   include nothing else except bsd.port.mk and bsd.port.pre.mk,
#     bsd.port.post.mk, bsd.port.options.mk
#   contain no dotted number, and not the driver version string
#   not assign NVIDIA_DISTVERSION
# Any other conditional, .else, .for, or dot-directive is refused.
# The two version .error guards must be opened at the top level.
# Assignments other than the skeleton variables are refused, so
# PORTVERSION and DISTVERSIONSUFFIX cannot rewrite the package version.
# A skeleton assignment must use operator =. Its value must not contain
# $, !, or a backtick. PORTSDIR may be set only to /usr/ports, and only
# inside .if !defined(PORTSDIR). Any other port Makefile line is refused.
# A version-file line that is not an NVIDIA_DISTVERSION assignment is
# refused. A dot-directive, +=, !=, or any value containing "$" or "\"
# fails here. Those lines are not ignored.
# A line continues only when it ends in an odd number of backslashes,
# matching bmake. An even number leaves the next line on its own.
# "#" starts a comment unless a backslash escapes it, or it follows "[".
# A .error message may expand ${PORTSDIR}. :!, :sh, and backticks are refused.
#
# The two trees are fixed paths, not $HOME:
#   /home/mlapointe/git/cloudbsd-ports-overlay
#   /home/mlapointe/git/freebsd-ports
# --self-test copies those trees and changes one thing at a time.
# --fixture is only for that self-test.

set -eu

root_overlay=/home/mlapointe/git/cloudbsd-ports-overlay
root_ports=/home/mlapointe/git/freebsd-ports

fail() {
	printf 'FAIL: %s\n' "$1" >&2
	exit 1
}

parse_driver() {
	awk -f - "$1" <<'AWK'
function die(msg) {
	if (died) exit 1
	died = 1
	print "ERROR " msg
	exit 1
}
function strip_comment(s,    i, c, q, out, prev) {
	q = ""
	out = ""
	prev = ""
	i = 1
	while (i <= length(s)) {
		c = substr(s, i, 1)
		if (q == "" && c == "\\") {
			out = out c
			i++
			if (i <= length(s)) {
				c = substr(s, i, 1)
				out = out c
				prev = c
				i++
			}
			continue
		}
		if (q == "" && c == "#" && prev != "[") break
		if (q == "" && (c == "\"" || c == "'")) q = c
		else if (c == q) q = ""
		out = out c
		prev = c
		i++
	}
	sub(/[[:space:]]+$/, "", out)
	return out
}
function trailing_bs(s,    i, n) {
	n = 0
	i = length(s)
	while (i >= 1 && substr(s, i, 1) == "\\") {
		n++
		i--
	}
	return n
}
function continued(s,    n, i, keep) {
	n = trailing_bs(s)
	if (n % 2 == 0) {
		piece = s
		return 0
	}
	keep = ""
	for (i = 0; i < int(n / 2); i++)
		keep = keep "\\"
	piece = substr(s, 1, length(s) - n) keep
	return 1
}
function take_op(s) {
	if (substr(s, 1, 2) == "?=") { op = "?="; val = substr(s, 3); return 1 }
	if (substr(s, 1, 2) == ":=") { op = ":="; val = substr(s, 3); return 1 }
	if (substr(s, 1, 2) == "+=") { op = "+="; val = substr(s, 3); return 1 }
	if (substr(s, 1, 2) == "!=") { op = "!="; val = substr(s, 3); return 1 }
	if (substr(s, 1, 1) == "=") { op = "="; val = substr(s, 2); return 1 }
	return 0
}
function assign_line(line,    name, rest) {
	name = "NVIDIA_DISTVERSION"
	if (substr(line, 1, length(name)) != name) return 0
	rest = substr(line, length(name) + 1, 1)
	if (rest != "" && rest !~ /[[:space:]=?:!+]/) return 0
	rest = substr(line, length(name) + 1)
	sub(/^[[:space:]]+/, "", rest)
	if (!take_op(rest)) die("unhandled NVIDIA_DISTVERSION assignment: " line)
	sub(/^[[:space:]]+/, "", val)
	sub(/[[:space:]]+$/, "", val)
	if (val ~ /\\/) die("cannot evaluate backslash")
	if (val ~ /\$/) die("cannot expand " op)
	if (op == "+=") die("cannot evaluate +=")
	if (op == "!=") die("cannot expand !=")
	if (op == "?=") {
		if (!defined) { value = val; defined = 1 }
	} else {
		value = val
		defined = 1
	}
	return 1
}
{
	if (continued($0)) { buf = buf piece; next }
	line = strip_comment(buf piece)
	buf = ""
	sub(/^[[:space:]]+/, "", line)
	sub(/[[:space:]]+$/, "", line)
	if (line == "") next
	if (substr(line, 1, 1) == ".") die("version file directive is not evaluated: " line)
	if (!assign_line(line)) die("version file line is not an NVIDIA_DISTVERSION assignment: " line)
}
END {
	if (died) exit 1
	if (buf != "") die("unterminated continuation in the version file")
	if (!defined) { print "UNSET"; exit 0 }
	if (value == "") { print "EMPTY"; exit 0 }
	print "VALUE " value
}
AWK
}

check_port() {
	awk -f - "$1" <<'AWK'
BEGIN {
	depth = 0
	n_dist = 0
	n_ver = 0
	n_bsd = 0
}
function die(msg) {
	if (died) exit 1
	died = 1
	print msg
	exit 1
}
function strip_comment(s,    i, c, q, out, prev) {
	q = ""
	out = ""
	prev = ""
	i = 1
	while (i <= length(s)) {
		c = substr(s, i, 1)
		if (q == "" && c == "\\") {
			out = out c
			i++
			if (i <= length(s)) {
				c = substr(s, i, 1)
				out = out c
				prev = c
				i++
			}
			continue
		}
		if (q == "" && c == "#" && prev != "[") break
		if (q == "" && (c == "\"" || c == "'")) q = c
		else if (c == q) q = ""
		out = out c
		prev = c
		i++
	}
	sub(/[[:space:]]+$/, "", out)
	return out
}
function trailing_bs(s,    i, n) {
	n = 0
	i = length(s)
	while (i >= 1 && substr(s, i, 1) == "\\") {
		n++
		i--
	}
	return n
}
function continued(s,    n, i, keep) {
	n = trailing_bs(s)
	if (n % 2 == 0) {
		piece = s
		return 0
	}
	keep = ""
	for (i = 0; i < int(n / 2); i++)
		keep = keep "\\"
	piece = substr(s, 1, length(s) - n) keep
	return 1
}
function dotted(s,    start, n, i, c) {
	if (!match(s, /[0-9]+\.[0-9]+/)) return ""
	start = RSTART
	n = RLENGTH
	i = start + n
	while (i <= length(s)) {
		c = substr(s, i, 1)
		if (c !~ /[0-9.]/) break
		n++
		i++
	}
	return substr(s, start, n)
}
function is_name(line, name,    rest) {
	if (substr(line, 1, length(name)) != name) return 0
	rest = substr(line, length(name) + 1, 1)
	if (rest != "" && rest !~ /[[:space:]=?:!+]/) return 0
	assign_rest = substr(line, length(name) + 1)
	sub(/^[[:space:]]+/, "", assign_rest)
	return 1
}
function take_op(s) {
	if (substr(s, 1, 2) == "?=") { op = "?="; val = substr(s, 3); return 1 }
	if (substr(s, 1, 2) == ":=") { op = ":="; val = substr(s, 3); return 1 }
	if (substr(s, 1, 2) == "+=") { op = "+="; val = substr(s, 3); return 1 }
	if (substr(s, 1, 2) == "!=") { op = "!="; val = substr(s, 3); return 1 }
	if (substr(s, 1, 1) == "=") { op = "="; val = substr(s, 2); return 1 }
	return 0
}
function open_if(cond) {
	if (cond == "!exists(${PORTSDIR}/x11/nvidia-driver/Makefile.version)" || \
	    cond == "empty(NVIDIA_DISTVERSION)") {
		if (depth != 0) die("required guard must be at top level: [" cond "]")
		if (cond == "!exists(${PORTSDIR}/x11/nvidia-driver/Makefile.version)" && n_ver != 0)
			die("missing-file guard must come before the driver version .include")
		if (cond == "empty(NVIDIA_DISTVERSION)" && n_ver == 0)
			die("empty-version guard must come after the driver version .include")
	} else if (cond == "!defined(PORTSDIR)" || cond == "empty(PORTSDIR)") {
		if (depth != 0) die("conditional not allowed at this depth: [" cond "]")
	} else die("conditional not allowed: [" cond "]")
	depth++
	stack[depth] = cond
	seen[cond] = 1
}
function word_of(line) {
	w = line
	sub(/[[:space:]].*$/, "", w)
	return w
}
function rest_of(line, w,    r) {
	r = substr(line, length(w) + 1)
	sub(/^[[:space:]]+/, "", r)
	return r
}
function handle(line,    w, cond, target) {
	w = word_of(line)
	if (substr(w, 1, 1) == ".") {
		if (w == ".if") {
			cond = rest_of(line, w)
			open_if(cond)
			return
		}
		if (w == ".elif") {
			cond = rest_of(line, w)
			if (depth != 1 || cond != "empty(PORTSDIR)" || stack[depth] != "!defined(PORTSDIR)")
				die("elif is only allowed for empty(PORTSDIR) under the top-level !defined(PORTSDIR)")
			stack[depth] = cond
			seen[cond] = 1
			return
		}
		if (w == ".endif") {
			if (depth == 0) die("endif without if")
			delete stack[depth]
			depth--
			return
		}
		if (w == ".else") die("remove .else from the port Makefile")
		if (w == ".for") die("remove .for from the port Makefile")
		if (w == ".error") {
			# bmake expands the message (parse.c HandleMessage, Var_Subst).
			# ${PORTSDIR} is the port's own missing-file text. :!, :sh, and
			# backticks run a shell. A backslash can hide the rest of the line.
			emsg = rest_of(line, w)
			if (emsg ~ /[!`]|:[sS][hH]/) die("value of .error contains a make expansion")
			if (emsg ~ /\\/) die("value of .error contains a backslash")
			if (depth > 0) erred[stack[depth]] = 1
			return
		}
		if (w == ".include") {
			target = rest_of(line, w)
			if (target == "\"${PORTSDIR}/x11/nvidia-driver/Makefile.version\"") {
				if (depth != 0) die("driver version .include is inside a conditional")
				n_ver++
				return
			}
			if (target == "<bsd.port.mk>") {
				if (depth != 0) die("bsd.port.mk .include is inside a conditional")
				n_bsd++
				return
			}
			if (target == "<bsd.port.pre.mk>" || target == "<bsd.port.post.mk>" || \
			    target == "<bsd.port.options.mk>") {
				if (depth != 0) die("ports framework .include is inside a conditional")
				return
			}
			die("include is not allowed: [" target "]")
		}
		die("directive not evaluated: " line)
	}
	if (is_name(line, "NVIDIA_DISTVERSION"))
		die("port Makefile assigns NVIDIA_DISTVERSION")
	if (is_name(line, "DISTVERSION")) {
		if (!take_op(assign_rest)) die("unhandled DISTVERSION assignment: " line)
		sub(/^[[:space:]]+/, "", val)
		sub(/[[:space:]]+$/, "", val)
		n_dist++
		if (n_dist == 1) { dist_op = op; dist_val = val; dist_depth = depth }
		return
	}
	if (match(line, /^[A-Za-z_][A-Za-z0-9_]*/)) {
		vname = substr(line, 1, RLENGTH)
		rest = substr(line, RLENGTH + 1)
		sub(/^[[:space:]]+/, "", rest)
		if (substr(rest, 1, 1) ~ /[=?:!+]/) {
			if (!allowed_var(vname)) die("assignment not allowed: [" vname "]")
			if (!take_op(rest)) die("unhandled assignment: " line)
			sub(/^[[:space:]]+/, "", val)
			sub(/[[:space:]]+$/, "", val)
			if (op != "=") die("only = is allowed on [" vname "], found " op)
			if (val ~ /[$!`]/) die("value of [" vname "] contains a make expansion")
			if (val ~ /\\/) die("value of [" vname "] contains a backslash")
			if (vname == "PORTSDIR") {
				if (depth != 1 || stack[depth] != "!defined(PORTSDIR)" || val != "/usr/ports")
					die("PORTSDIR may only be set to /usr/ports inside !defined(PORTSDIR)")
			}
			return
		}
	}
	die("line is not allowed in the port Makefile: " line)
}
function allowed_var(n) {
	return n == "PORTNAME" || n == "CATEGORIES" || n == "MAINTAINER" || \
	    n == "CLOUDBSD_OWNER" || n == "COMMENT" || n == "WWW" || n == "LICENSE" || \
	    n == "ONLY_FOR_ARCHS" || n == "ONLY_FOR_ARCHS_REASON" || n == "PORTSDIR" || \
	    n == "BROKEN"
}
{
	if (continued($0)) { buf = buf piece; next }
	raw = buf piece
	buf = ""
	tok = dotted(raw)
	if (tok != "") die("literal version number [" tok "] in the port Makefile. Delete it and use ${NVIDIA_DISTVERSION}")
	line = strip_comment(raw)
	sub(/^[[:space:]]+/, "", line)
	sub(/[[:space:]]+$/, "", line)
	if (line == "") next
	handle(line)
}
END {
	if (died) exit 1
	if (buf != "") die("unterminated continuation in the port Makefile")
	if (depth != 0) die("unbalanced conditional")
	if (n_dist != 1)
		die("the port Makefile has " n_dist " DISTVERSION assignments. Leave one: DISTVERSION=${NVIDIA_DISTVERSION}")
	if (dist_op != "=" || dist_val != "${NVIDIA_DISTVERSION}" || dist_depth != 0)
		die("DISTVERSION must use operator = and value ${NVIDIA_DISTVERSION} at top level, found " dist_op " [" dist_val "] at depth " dist_depth)
	if (!seen["!exists(${PORTSDIR}/x11/nvidia-driver/Makefile.version)"])
		die("missing .if !exists(${PORTSDIR}/x11/nvidia-driver/Makefile.version)")
	if (!erred["!exists(${PORTSDIR}/x11/nvidia-driver/Makefile.version)"])
		die("missing .error under !exists(${PORTSDIR}/x11/nvidia-driver/Makefile.version)")
	if (!seen["empty(NVIDIA_DISTVERSION)"])
		die("missing .if empty(NVIDIA_DISTVERSION)")
	if (!erred["empty(NVIDIA_DISTVERSION)"])
		die("missing .error under empty(NVIDIA_DISTVERSION)")
	if (n_ver != 1)
		die("driver version .include count is " n_ver ", want 1")
	if (n_bsd != 1)
		die("bsd.port.mk .include count is " n_bsd ", want 1")
}
AWK
}

run_check() {
	overlay=${OVERLAY:-$root_overlay}
	ports=${PORTS:-$root_ports}
	fixture=0
	if [ "${1:-}" = "--fixture" ]; then
		fixture=1
	fi

	overlay_real=$(realpath -e "$overlay" 2>/dev/null) || fail "OVERLAY does not resolve: $overlay"
	ports_real=$(realpath -e "$ports" 2>/dev/null) || fail "PORTS does not resolve: $ports"
	want_overlay=$(realpath -e "$root_overlay") || fail "missing $root_overlay"
	want_ports=$(realpath -e "$root_ports") || fail "missing $root_ports"
	if [ "$fixture" -eq 0 ]; then
		[ "$overlay_real" = "$want_overlay" ] || fail "OVERLAY resolves to $overlay_real, not $want_overlay. Unset OVERLAY."
		[ "$ports_real" = "$want_ports" ] || fail "PORTS resolves to $ports_real, not $want_ports. Unset PORTS."
		pathkind=canonical
	else
		pathkind=fixture
	fi

	verfile=$ports_real/x11/nvidia-driver/Makefile.version
	portmk=$overlay_real/graphics/nvidia-uvm-kmod/Makefile
	# A later swap of this path is out of scope. The caller already owns both trees.
	ver_real=$(realpath -e "$verfile" 2>/dev/null) || fail "missing driver version file $verfile"
	port_real=$(realpath -e "$portmk" 2>/dev/null) || fail "missing $portmk"
	case "$ver_real" in
	"$ports_real"/*) ;;
	*) fail "version file resolves outside PORTS: $ver_real" ;;
	esac
	case "$port_real" in
	"$overlay_real"/*) ;;
	*) fail "port Makefile resolves outside OVERLAY: $port_real" ;;
	esac
	verfile=$ver_real
	portmk=$port_real
	[ -r "$verfile" ] || fail "cannot read $verfile"
	[ -r "$portmk" ] || fail "cannot read $portmk"
	rc=0
	parsed=$(parse_driver "$verfile") || rc=$?
	tag=${parsed%% *}
	rest=${parsed#* }
	if [ "$tag" = "ERROR" ]; then
		fail "$rest"
	fi
	if [ "$rc" -ne 0 ]; then
		fail "could not read $verfile (parser status $rc) [$parsed]"
	fi
	case "$tag" in
	VALUE) driver=$rest ;;
	EMPTY) fail "NVIDIA_DISTVERSION is empty in $verfile. Set NVIDIA_DISTVERSION in that file. Do not copy a number into the port." ;;
	UNSET) fail "NVIDIA_DISTVERSION is unset in $verfile. Set it there; do not copy a number into the port." ;;
	*) fail "version parser returned [$parsed]" ;;
	esac
	case "$driver" in
	*[!0-9.]*) fail "NVIDIA_DISTVERSION is not a dotted number: [$driver]" ;;
	*.*) ;;
	*) fail "NVIDIA_DISTVERSION is not a dotted number: [$driver]" ;;
	esac
	case "$driver" in
	.*|*.) fail "NVIDIA_DISTVERSION is not a dotted number: [$driver]" ;;
	*..*) fail "NVIDIA_DISTVERSION is not a dotted number: [$driver]" ;;
	esac

	[ -f "$portmk" ] || fail "missing $portmk"
	if grep -q 'NVIDIA_DRIVER_VERSION_MK' "$portmk"; then
		fail "remove NVIDIA_DRIVER_VERSION_MK. It lets a caller point the version at another file."
	fi
	if grep -F -q -- "$driver" "$portmk"; then
		fail "port Makefile contains the driver version $driver. Delete that copy."
	fi
	rc=0
	msg=$(check_port "$portmk") || rc=$?
	if [ "$rc" -ne 0 ]; then
		[ -n "$msg" ] || fail "could not read $portmk (checker status $rc)"
		fail "$msg"
	fi

	printf 'PASS: %s paths. text only, make was not run. driver NVIDIA_DISTVERSION=%s from %s. port sets one top-level DISTVERSION=${NVIDIA_DISTVERSION}, .errors when the version file is missing and when NVIDIA_DISTVERSION is empty.\n' \
		"$pathkind" "$driver" "$verfile"
}

self_test() {
	self=$(realpath "$0")
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT
	base=$tmp/base
	work=$tmp/work
	mkdir -p "$base/overlay/graphics/nvidia-uvm-kmod" "$base/ports/x11/nvidia-driver"
	cp "$root_overlay/graphics/nvidia-uvm-kmod/Makefile" "$base/overlay/graphics/nvidia-uvm-kmod/Makefile"
	cp "$root_ports/x11/nvidia-driver/Makefile.version" "$base/ports/x11/nvidia-driver/Makefile.version"
	tok=$(grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)*' "$root_ports/x11/nvidia-driver/Makefile.version") || fail "driver version file has no dotted number"
	ntok=$(printf '%s\n' "$tok" | grep -c .)
	[ "$ntok" -eq 1 ] || fail "driver version file has $ntok dotted numbers; the self-test oracle wants one"

	prepare() {
		rm -rf "$work"
		mkdir -p "$work"
		cp -R "$base/overlay" "$work/overlay"
		cp -R "$base/ports" "$work/ports"
		mk=$work/overlay/graphics/nvidia-uvm-kmod/Makefile
		vf=$work/ports/x11/nvidia-driver/Makefile.version
	}
	run_fix() {
		_rc=0
		_out=$(env VERSION_LOCK_FIXTURE=1 OVERLAY="$work/overlay" PORTS="$work/ports" \
			/bin/sh "$self" --fixture 2>&1) || _rc=$?
	}
	show() {
		printf 'case %s: exit=%s\n%s\n' "$1" "$_rc" "$_out"
	}
	expect_fail() {
		_name=$1
		_need=$2
		run_fix
		show "$_name"
		if [ "$_rc" -eq 0 ]; then
			printf 'SELF-TEST FAIL: %s exited 0\n' "$_name" >&2
			exit 1
		fi
		if ! printf '%s\n' "$_out" | grep -F -q -- "$_need"; then
			printf 'SELF-TEST FAIL: %s did not report [%s]\n' "$_name" "$_need" >&2
			exit 1
		fi
		if printf '%s\n' "$_out" | grep -F -q 'PASS:'; then
			printf 'SELF-TEST FAIL: %s printed PASS\n' "$_name" >&2
			exit 1
		fi
	}
	expect_pass() {
		_name=$1
		_need=$2
		run_fix
		show "$_name"
		if [ "$_rc" -ne 0 ]; then
			printf 'SELF-TEST FAIL: %s exited %s\n' "$_name" "$_rc" >&2
			exit 1
		fi
		if ! printf '%s\n' "$_out" | grep -F -q -- "$_need"; then
			printf 'SELF-TEST FAIL: %s did not report [%s]\n' "$_name" "$_need" >&2
			exit 1
		fi
	}
	absent() {
		if printf '%s\n' "$_out" | grep -F -q -- "$1"; then
			printf 'SELF-TEST FAIL: output contained [%s]\n' "$1" >&2
			exit 1
		fi
	}

	printf 'version-lock self-test\ntext only; make was not run\n\n'

	prepare
	sed 's/^\.error x11\/nvidia-driver/# .error x11\/nvidia-driver/' "$mk" > "$mk.n"
	mv "$mk.n" "$mk"
	grep -F -q '# .error x11/nvidia-driver' "$mk" || fail "mutation did not land: comment exists .error"
	expect_fail "commented missing-file .error" 'missing .error under !exists(${PORTSDIR}/x11/nvidia-driver/Makefile.version)'

	prepare
	sed 's/^\.error NVIDIA_DISTVERSION/# .error NVIDIA_DISTVERSION/' "$mk" > "$mk.n"
	mv "$mk.n" "$mk"
	grep -F -q '# .error NVIDIA_DISTVERSION' "$mk" || fail "mutation did not land: comment empty .error"
	expect_fail "commented empty-version .error" 'missing .error under empty(NVIDIA_DISTVERSION)'

	prepare
	sed 's/^\.error /# .error /' "$mk" > "$mk.n"
	mv "$mk.n" "$mk"
	printf '%s\n' '.error unrelated' >> "$mk"
	grep -F -q '.error unrelated' "$mk" || fail "mutation did not land: unrelated .error"
	live=$(awk '/^\.error / { c++ } END { print c+0 }' "$mk")
	[ "$live" -eq 1 ] || fail "mutation did not leave exactly one live .error (have $live)"
	expect_fail "only an unrelated .error" 'missing .error under !exists(${PORTSDIR}/x11/nvidia-driver/Makefile.version)'

	prepare
	printf '\n%s\n' 'DISTVERSION=${NVIDIA_DISTVERSION}' >> "$mk"
	expect_fail "second DISTVERSION" "the port Makefile has 2 DISTVERSION assignments"

	prepare
	sed 's/^DISTVERSION=[[:space:]]*\${NVIDIA_DISTVERSION}$/DISTVERSION=${NVIDIA_DISTVERSION}.1/' "$mk" > "$mk.n"
	mv "$mk.n" "$mk"
	grep -F -q 'DISTVERSION=${NVIDIA_DISTVERSION}.1' "$mk" || fail "mutation did not land: DISTVERSION suffix"
	expect_fail "DISTVERSION suffix" 'found = [${NVIDIA_DISTVERSION}.1]'

	prepare
	sed 's/^DISTVERSION=/DISTVERSION?=/' "$mk" > "$mk.n"
	mv "$mk.n" "$mk"
	grep -F -q 'DISTVERSION?=' "$mk" || fail "mutation did not land: DISTVERSION?="
	expect_fail "DISTVERSION ?=" 'found ?= [${NVIDIA_DISTVERSION}]'

	prepare
	awk 'index($0, ".include \"${PORTSDIR}/x11/nvidia-driver/Makefile.version\"") == 1 {
		print ".if 0"; print; print ".endif"; next } { print }' "$mk" > "$mk.n"
	mv "$mk.n" "$mk"
	grep -F -q '.if 0' "$mk" || fail "mutation did not land: .if 0"
	expect_fail ".if 0 around include" "conditional not allowed: [0]"

	prepare
	awk 'index($0, ".include \"${PORTSDIR}/x11/nvidia-driver/Makefile.version\"") == 1 {
		print ".if defined(UNUSED)"; print; print ".endif"; next } { print }' "$mk" > "$mk.n"
	mv "$mk.n" "$mk"
	grep -F -q '.if defined(UNUSED)' "$mk" || fail "mutation did not land: defined(UNUSED)"
	expect_fail ".if defined(UNUSED)" "conditional not allowed: [defined(UNUSED)]"

	prepare
	awk '/^\.if !defined\(PORTSDIR\)/ {
		print; print ".include \"${PORTSDIR}/x11/nvidia-driver/Makefile.version\""; next }
		index($0, ".include \"${PORTSDIR}/x11/nvidia-driver/Makefile.version\"") == 1 { next }
		{ print }' "$mk" > "$mk.n"
	mv "$mk.n" "$mk"
	ninc=$(awk 'index($0, ".include \"${PORTSDIR}/x11/nvidia-driver/Makefile.version\"") == 1 { c++ } END { print c+0 }' "$mk")
	[ "$ninc" -eq 1 ] || fail "mutation did not leave one driver include (have $ninc)"
	expect_fail "include inside !defined(PORTSDIR)" "driver version .include is inside a conditional"

	prepare
	printf '%s\n' '.include "${OTHER_MK}"' >> "$mk"
	grep -F -q '.include "${OTHER_MK}"' "$mk" || fail "mutation did not land: OTHER_MK"
	expect_fail "other include" "include is not allowed:"

	prepare
	printf '%s\n' '.for x in 1' >> "$mk"
	grep -F -q '.for x in 1' "$mk" || fail "mutation did not land: .for"
	expect_fail ".for" "remove .for from the port Makefile"

	prepare
	printf '%s\n' 'FOO=bar # 470.86' >> "$mk"
	grep -F -q 'FOO=bar # 470.86' "$mk" || fail "mutation did not land: trailing comment"
	expect_fail "dotted number in a trailing comment" "literal version number [470.86]"

	prepare
	awk 'BEGIN { print "EXTRA=470.\\"; print "86" }' >> "$mk"
	cont=$(awk '/\\$/ { c++ } END { print c+0 }' "$mk")
	[ "$cont" -eq 1 ] || fail "mutation did not land: continued dotted number (continuations $cont)"
	expect_fail "continued dotted number" "literal version number [470.86]"

	prepare
	printf '%s\n' 'NVIDIA_DISTVERSION=latest' >> "$mk"
	grep -F -q 'NVIDIA_DISTVERSION=latest' "$mk" || fail "mutation did not land: NVIDIA_DISTVERSION=latest"
	expect_fail "port assigns NVIDIA_DISTVERSION" "port Makefile assigns NVIDIA_DISTVERSION"

	prepare
	awk 'index($0, ".include \"${PORTSDIR}/x11/nvidia-driver/Makefile.version\"") == 1 {
		print ".include \\"; print "\t\"${PORTSDIR}/x11/nvidia-driver/Makefile.version\""; next }
		{ print }' "$mk" > "$mk.n"
	mv "$mk.n" "$mk"
	cont=$(awk '/\\$/ { c++ } END { print c+0 }' "$mk")
	[ "$cont" -eq 1 ] || fail "mutation did not land: continued include (continuations $cont)"
	expect_pass "continued legal include" "fixture paths"

	prepare
	printf '%s\n' '.error unrelated' >> "$mk"
	grep -F -q '.error unrelated' "$mk" || fail "mutation did not land: extra .error"
	expect_pass "extra top-level .error" "fixture paths"

	prepare
	printf '%s\n' 'NVIDIA_DISTVERSION=' 'NVIDIA_DISTVERSION?=9.9.9' > "$vf"
	grep -F -q 'NVIDIA_DISTVERSION?=' "$vf" || fail "mutation did not land: empty ?="
	expect_fail "defined-empty then ?=" "NVIDIA_DISTVERSION is empty"
	absent '9.9.9'

	prepare
	printf '%s\n' 'NVIDIA_DISTVERSION?=1.1.1' 'NVIDIA_DISTVERSION=2.2.2' > "$vf"
	expect_pass "later = wins" "NVIDIA_DISTVERSION=2.2.2"
	absent '1.1.1'

	prepare
	printf '%s\n' 'NVIDIA_DISTVERSION=2.2.2' 'NVIDIA_DISTVERSION?=1.1.1' > "$vf"
	expect_pass "later ?= does not win" "NVIDIA_DISTVERSION=2.2.2"
	absent '1.1.1'

	prepare
	printf '%s\n' 'NVIDIA_DISTVERSION = 7.7.7' > "$vf"
	expect_pass "spaces around =" "NVIDIA_DISTVERSION=7.7.7"

	prepare
	printf '%s\n' 'NVIDIA_DISTVERSION:=6.6.6' > "$vf"
	expect_pass ":= literal" "NVIDIA_DISTVERSION=6.6.6"

	prepare
	printf '%s\n' 'NVIDIA_DISTVERSION+=6.6' > "$vf"
	expect_fail "+=" "cannot evaluate +="

	prepare
	printf '%s\n' 'NVIDIA_DISTVERSION!=echo 8.8.8' > "$vf"
	expect_fail "!=" "cannot expand !="
	absent '8.8.8'

	prepare
	awk 'BEGIN { print "NVIDIA_DISTVERSION?=3.3.\\"; print "3" }' > "$vf"
	cont=$(awk '/\\$/ { c++ } END { print c+0 }' "$vf")
	[ "$cont" -eq 1 ] || fail "mutation did not land: continued version (continuations $cont)"
	expect_pass "continued version value" "NVIDIA_DISTVERSION=3.3.3"

	prepare
	printf '%s\n' '.if !defined(PORTSDIR)' '.if empty(NVIDIA_DISTVERSION)' '.error nested' '.endif' '.endif' >> "$mk"
	grep -F -q '.error nested' "$mk" || fail "mutation did not land: nested guard"
	expect_fail "nested required guard" 'required guard must be at top level: [empty(NVIDIA_DISTVERSION)]'

	prepare
	printf '%s\n' '.if 0' 'NVIDIA_DISTVERSION=1.2.3' '.endif' 'NVIDIA_DISTVERSION?=4.4.4' > "$vf"
	grep -F -q '.if 0' "$vf" || fail "mutation did not land: version file .if"
	expect_fail "version file .if" "version file directive is not evaluated"
	absent '1.2.3'

	prepare
	printf '%s\n' 'NVIDIA_DISTVERSION?=5.5.5' 'NVIDIA_DISTVERSION?=${OTHER}' > "$vf"
	grep -F -q '${OTHER}' "$vf" || fail "mutation did not land: dollar ?="
	expect_fail "conditional ?= with a dollar" "cannot expand ?="
	absent '5.5.5'

	prepare
	printf '%s\n' 'PORTVERSION=1' >> "$mk"
	grep -F -q 'PORTVERSION=1' "$mk" || fail "mutation did not land: PORTVERSION"
	expect_fail "PORTVERSION" "assignment not allowed: [PORTVERSION]"

	prepare
	printf '%s\n' 'DISTVERSIONSUFFIX=.9' >> "$mk"
	grep -F -q 'DISTVERSIONSUFFIX=.9' "$mk" || fail "mutation did not land: DISTVERSIONSUFFIX"
	expect_fail "DISTVERSIONSUFFIX" "assignment not allowed: [DISTVERSIONSUFFIX]"

	prepare
	printf '%s\n' 'NVIDIA_DISTVERSION?=1.2.3' 'FOO!=echo hi' > "$vf"
	grep -F -q 'FOO!=echo hi' "$vf" || fail "mutation did not land: version file FOO!="
	expect_fail "version file shell assignment" "version file line is not an NVIDIA_DISTVERSION assignment"
	absent '1.2.3'

	prepare
	printf '%s\n' 'PORTNAME!=id' >> "$mk"
	grep -F -q 'PORTNAME!=id' "$mk" || fail "mutation did not land: PORTNAME!="
	expect_fail "PORTNAME shell assignment" "only = is allowed on [PORTNAME], found !="

	prepare
	printf '%s\n' 'COMMENT=${:!id!}' >> "$mk"
	grep -F -q 'COMMENT=${:!id!}' "$mk" || fail "mutation did not land: COMMENT expansion"
	expect_fail "skeleton value expansion" "value of [COMMENT] contains a make expansion"

	prepare
	printf '%s\n' 'PORTSDIR=/tmp/evil' >> "$mk"
	grep -F -q 'PORTSDIR=/tmp/evil' "$mk" || fail "mutation did not land: top-level PORTSDIR"
	expect_fail "top-level PORTSDIR" "PORTSDIR may only be set to /usr/ports inside !defined(PORTSDIR)"

	prepare
	printf '%s\n' 'echo hi' >> "$mk"
	grep -F -q 'echo hi' "$mk" || fail "mutation did not land: recipe line"
	expect_fail "recipe line" "line is not allowed in the port Makefile"

	prepare
	printf '%s\n' '# comment \\' '.include "/tmp/evil.mk"' >> "$mk"
	grep -F -q '# comment \\' "$mk" || fail "mutation did not land: even backslash"
	expect_fail "even backslash hides an include" 'include is not allowed: ["/tmp/evil.mk"]'

	prepare
	printf '%s\n' 'PORTNAME=x\\' >> "$mk"
	grep -F -q 'PORTNAME=x\\' "$mk" || fail "mutation did not land: port backslash value"
	expect_fail "even backslashes in a value" "value of [PORTNAME] contains a backslash"

	prepare
	printf '%s\n' 'NVIDIA_DISTVERSION=1.2.3' '# comment \\' 'FOO!=echo hi' > "$vf"
	grep -F -q '# comment \\' "$vf" || fail "mutation did not land: version comment backslash"
	expect_fail "version comment hides a shell line" "version file line is not an NVIDIA_DISTVERSION assignment"
	absent '1.2.3'

	prepare
	printf '%s\n' 'COMMENT=x\#${:!id!}' >> "$mk"
	grep -F -q 'COMMENT=x\#${:!id!}' "$mk" || fail "mutation did not land: escaped hash"
	expect_fail "escaped hash hides a shell" "value of [COMMENT] contains a make expansion"

	prepare
	printf '%s\n' 'COMMENT=x:[#]${:!id!}' >> "$mk"
	grep -F -q 'COMMENT=x:[#]${:!id!}' "$mk" || fail "mutation did not land: bracket hash"
	expect_fail "bracket hash hides a shell" "value of [COMMENT] contains a make expansion"

	prepare
	printf '%s\n' 'COMMENT=x\#plain' >> "$mk"
	grep -F -q 'COMMENT=x\#plain' "$mk" || fail "mutation did not land: backslash value"
	expect_fail "backslash in a skeleton value" "value of [COMMENT] contains a backslash"

	prepare
	printf '%s\n' '.error ${:!id!}' >> "$mk"
	grep -F -q '.error ${:!id!}' "$mk" || fail "mutation did not land: error shell"
	expect_fail "error message runs a shell" "value of .error contains a make expansion"
	absent 'id'

	prepare
	printf '%s\n' '.error ${UNSET:sh}' >> "$mk"
	grep -F -q '.error ${UNSET:sh}' "$mk" || fail "mutation did not land: error :sh"
	expect_fail "error message :sh" "value of .error contains a make expansion"

	prepare
	printf '%s\n' '.error has a \ slash' >> "$mk"
	grep -F -q '.error has a \ slash' "$mk" || fail "mutation did not land: error backslash"
	expect_fail "backslash in .error" "value of .error contains a backslash"

	prepare
	printf '%s\n' '.error missing ${PORTSDIR}' >> "$mk"
	grep -F -q '.error missing ${PORTSDIR}' "$mk" || fail "mutation did not land: error PORTSDIR"
	expect_pass "error may name PORTSDIR" "fixture paths"

	prepare
	awk 'BEGIN { print ".include \"${PORTSDIR}/x11/nvidia-driver/Makefile.version\"" }
		index($0, ".include \"${PORTSDIR}/x11/nvidia-driver/Makefile.version\"") == 1 { next }
		{ print }' "$mk" > "$mk.n"
	mv "$mk.n" "$mk"
	ninc=$(awk 'index($0, ".include \"${PORTSDIR}/x11/nvidia-driver/Makefile.version\"") == 1 { c++ } END { print c+0 }' "$mk")
	[ "$ninc" -eq 1 ] || fail "mutation did not leave one driver include (have $ninc)"
	expect_fail "include before missing-file guard" "missing-file guard must come before the driver version .include"

	prepare
	awk 'index($0, ".if empty(NVIDIA_DISTVERSION)") == 1 { grab = 1 }
		grab {
			block = block $0 ORS
			if ($0 ~ /^\.endif$/) grab = 0
			next
		}
		{ lines[++n] = $0 }
		END {
			for (i = 1; i <= n; i++) {
				if (index(lines[i], ".include \"${PORTSDIR}/x11/nvidia-driver/Makefile.version\"") == 1)
					printf "%s", block
				print lines[i]
			}
		}' "$mk" > "$mk.n"
	mv "$mk.n" "$mk"
	order=$(awk 'index($0, ".if empty(NVIDIA_DISTVERSION)") == 1 { e = NR }
		index($0, ".include \"${PORTSDIR}/x11/nvidia-driver/Makefile.version\"") == 1 { inc = NR }
		END { if (e > 0 && inc > 0 && e < inc) print "before"; else print "not-before" }' "$mk")
	[ "$order" = "before" ] || fail "mutation did not land: empty guard is $order the include"
	expect_fail "empty guard before include" "empty-version guard must come after the driver version .include"

	prepare
	cp "$mk" "$tmp/escaped.mk"
	rm "$mk"
	ln -s "$tmp/escaped.mk" "$mk"
	[ -L "$mk" ] || fail "mutation did not land: makefile symlink"
	expect_fail "makefile symlink leaves the overlay" "resolves outside"

	fake=$tmp/home
	mkdir -p "$fake/git/cloudbsd-ports-overlay/graphics/nvidia-uvm-kmod" \
		"$fake/git/freebsd-ports/x11/nvidia-driver"
	cp "$base/overlay/graphics/nvidia-uvm-kmod/Makefile" \
		"$fake/git/cloudbsd-ports-overlay/graphics/nvidia-uvm-kmod/Makefile"
	printf '%s\n' 'NVIDIA_DISTVERSION?=1.2.3' \
		> "$fake/git/freebsd-ports/x11/nvidia-driver/Makefile.version"
	_rc=0
	_out=$(env -u VERSION_LOCK_FIXTURE -u OVERLAY -u PORTS HOME="$fake" /bin/sh "$self" 2>&1) || _rc=$?
	show "HOME does not move the trees"
	if [ "$_rc" -ne 0 ]; then
		printf 'SELF-TEST FAIL: HOME case exited %s\n' "$_rc" >&2
		exit 1
	fi
	if ! printf '%s\n' "$_out" | grep -F -q '/home/mlapointe/git/freebsd-ports/x11/nvidia-driver/Makefile.version'; then
		printf 'SELF-TEST FAIL: HOME case did not read the fixed ports tree\n' >&2
		exit 1
	fi
	absent '1.2.3'

	gdir=$tmp/glob
	mkdir -p "$gdir"
	: > "$gdir/595.99.02"
	prepare
	printf '%s\n' 'NVIDIA_DISTVERSION=595.99.0[0-9]' > "$vf"
	grep -F -q '595.99.0[0-9]' "$vf" || fail "mutation did not land: bracket version"
	_rc=0
	_out=$(cd "$gdir" && env VERSION_LOCK_FIXTURE=1 OVERLAY="$work/overlay" PORTS="$work/ports" \
		/bin/sh "$self" --fixture 2>&1) || _rc=$?
	show "bracket version is not globbed"
	if [ "$_rc" -eq 0 ]; then
		printf 'SELF-TEST FAIL: bracket version exited 0\n' >&2
		exit 1
	fi
	if ! printf '%s\n' "$_out" | grep -F -q 'not a dotted number'; then
		printf 'SELF-TEST FAIL: bracket version was not rejected\n' >&2
		exit 1
	fi
	if printf '%s\n' "$_out" | grep -F -q 'PASS:'; then
		printf 'SELF-TEST FAIL: bracket version printed PASS\n' >&2
		exit 1
	fi

	_rc=0
	_out=$(env -u VERSION_LOCK_FIXTURE -u OVERLAY -u PORTS /bin/sh "$self" 2>&1) || _rc=$?
	show "canonical"
	if [ "$_rc" -ne 0 ]; then
		printf 'SELF-TEST FAIL: canonical exited %s\n' "$_rc" >&2
		exit 1
	fi
	if ! printf '%s\n' "$_out" | grep -F -q 'canonical paths'; then
		printf 'SELF-TEST FAIL: canonical did not say canonical paths\n' >&2
		exit 1
	fi
	if ! printf '%s\n' "$_out" | grep -F -q 'make was not run'; then
		printf 'SELF-TEST FAIL: canonical did not say make was not run\n' >&2
		exit 1
	fi
	if ! printf '%s\n' "$_out" | grep -F -q "NVIDIA_DISTVERSION=$tok"; then
		printf 'SELF-TEST FAIL: canonical version is not the dotted token %s\n' "$tok" >&2
		exit 1
	fi
	if printf '%s\n' "$_out" | grep -F -q 'fixture paths'; then
		printf 'SELF-TEST FAIL: canonical said fixture paths\n' >&2
		exit 1
	fi

	decoy=$tmp/decoy
	mkdir -p "$decoy"
	cp -R "$base/overlay" "$decoy/overlay"
	cp -R "$base/ports" "$decoy/ports"
	_rc=0
	_out=$(env -u VERSION_LOCK_FIXTURE OVERLAY="$decoy/overlay" PORTS="$root_ports" \
		/bin/sh "$self" 2>&1) || _rc=$?
	show "decoy OVERLAY"
	if [ "$_rc" -eq 0 ]; then
		printf 'SELF-TEST FAIL: decoy OVERLAY exited 0\n' >&2
		exit 1
	fi
	if ! printf '%s\n' "$_out" | grep -F -q "OVERLAY resolves to"; then
		printf 'SELF-TEST FAIL: decoy OVERLAY did not refuse the path\n' >&2
		exit 1
	fi
	if printf '%s\n' "$_out" | grep -F -q 'PASS:'; then
		printf 'SELF-TEST FAIL: decoy OVERLAY printed PASS\n' >&2
		exit 1
	fi

	_rc=0
	_out=$(env -u VERSION_LOCK_FIXTURE OVERLAY="$root_overlay" PORTS="$decoy/ports" \
		/bin/sh "$self" 2>&1) || _rc=$?
	show "decoy PORTS"
	if [ "$_rc" -eq 0 ]; then
		printf 'SELF-TEST FAIL: decoy PORTS exited 0\n' >&2
		exit 1
	fi
	if ! printf '%s\n' "$_out" | grep -F -q "PORTS resolves to"; then
		printf 'SELF-TEST FAIL: decoy PORTS did not refuse the path\n' >&2
		exit 1
	fi

	printf '\nself-test passed\n'
}

if [ "${1:-}" = "--self-test" ]; then
	[ "$#" -eq 1 ] || fail "unknown argument"
	self_test
	exit 0
fi
if [ "${1:-}" = "--fixture" ]; then
	[ "$#" -eq 1 ] || fail "unknown argument"
	[ "${VERSION_LOCK_FIXTURE:-}" = "1" ] || fail "--fixture requires VERSION_LOCK_FIXTURE=1"
	run_check --fixture
	exit 0
fi
if [ -n "${1:-}" ]; then
	fail "unknown argument ${1}. Run with no argument, or with --self-test."
fi
if [ -n "${VERSION_LOCK_FIXTURE:-}" ]; then
	fail "VERSION_LOCK_FIXTURE is set. Unset it. The canonical check does not take a fixture."
fi
run_check
