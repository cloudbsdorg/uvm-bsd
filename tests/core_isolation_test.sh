#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# core_isolation_test.sh - the vendor-neutral core of the UVM driver
# (sys/core/) stands on its own: every file under it compiles alone,
# includes nothing but FreeBSD's own headers and the core's own, and
# names no GPU vendor.
#
# The checker is proved with two mutants of a core header, which it must
# flag: one with a vendor header included, one with a vendor's name in a
# comment.  A green run therefore means the rules still fire.
#
# Run: sh tests/core_isolation_test.sh

set -eu
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
export PATH LC_ALL=C

TESTSDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$TESTSDIR/.." && pwd)
CORE="$REPO/sys/core"
WORK=${GB10_WORK:-$HOME/work/gb10}
mkdir -p "$WORK/tmp"
T=$(mktemp -d "$WORK/tmp/uvm-core-iso.XXXXXXXX")
trap 'rm -rf "$T"' EXIT HUP INT QUIT TERM

rc=0

# The words a core file must not contain.  The first group is matched as a
# plain substring, the second as a whole word (case sensitive), the third
# as the beginning of an identifier, so that neither a vendor's name nor a
# compound name built on the vendor's own prefixes can hide.
FORBIDDEN_SUBSTRINGS='nvidia|NVIDIA|rm_gpu|Blackwell|GPFIFO|PTE|pushbuffer'
FORBIDDEN_WORDS='Nv|RM'
FORBIDDEN_PREFIXES='(^|[^A-Za-z0-9_])(Nv|RM)[A-Za-z_]'

# isolation_check FILE
#
# Print every violation of the isolation rules of FILE, one per line, and
# return 0 when the file is clean.
isolation_check() {
	f=$1
	violations=0

	# the includes: a core file includes the FreeBSD headers and its own
	grep -n '#[[:space:]]*include' "$f" > "$T/includes" || true
	while IFS= read -r hit; do
		[ -n "$hit" ] || continue
		lineno=${hit%%:*}
		text=${hit#*:}
		target=$(printf '%s\n' "$text" | sed -e 's/^[^<\"]*[<\"]//' -e 's/[>\"][[:space:]]*$//')
		case "$text" in
		*'<'*)
			# an angled include is a system header, never one of ours
			if [ -f "$REPO/sys/$target" ] && [ ! -f "$CORE/$target" ]; then
				printf '%s:%s: includes %s, which is not a system header\n' \
				    "$f" "$lineno" "$target"
				violations=$((violations + 1))
			fi
			;;
		*)
			# a quoted include is a core header, named without a path
			case "$target" in
			*/*)
				printf '%s:%s: includes %s, which is outside sys/core\n' \
				    "$f" "$lineno" "$target"
				violations=$((violations + 1))
				;;
			*)
				if [ ! -f "$CORE/$target" ]; then
					printf '%s:%s: includes %s, which is not a header of sys/core\n' \
					    "$f" "$lineno" "$target"
					violations=$((violations + 1))
				fi
				;;
			esac
			;;
		esac
	done < "$T/includes"

	# the words: a vendor's name never appears in a core file
	grep -n -E "$FORBIDDEN_SUBSTRINGS" "$f" > "$T/words" || true
	while IFS= read -r hit; do
		[ -n "$hit" ] || continue
		printf '%s:%s: forbidden word: %s\n' "$f" "${hit%%:*}" "${hit#*:}"
		violations=$((violations + 1))
	done < "$T/words"

	grep -n -w -E "$FORBIDDEN_WORDS" "$f" > "$T/words" || true
	while IFS= read -r hit; do
		[ -n "$hit" ] || continue
		printf '%s:%s: forbidden word: %s\n' "$f" "${hit%%:*}" "${hit#*:}"
		violations=$((violations + 1))
	done < "$T/words"

	grep -n -E "$FORBIDDEN_PREFIXES" "$f" > "$T/words" || true
	while IFS= read -r hit; do
		[ -n "$hit" ] || continue
		printf '%s:%s: forbidden word: %s\n' "$f" "${hit%%:*}" "${hit#*:}"
		violations=$((violations + 1))
	done < "$T/words"

	return $violations
}

# ---- the core itself ----
if [ ! -d "$CORE" ]; then
	echo "FAIL no files under sys/core"
	exit 1
fi

files=$(find "$CORE" -type f | LC_ALL=C sort)
if [ -z "$files" ]; then
	echo "FAIL no files under sys/core"
	exit 1
fi

for f in $files; do
	rel=${f#"$REPO"/}

	# (a) the file compiles on its own, as the kernel and userland see it
	if cc -std=c11 -Wall -Wextra -Werror -fsyntax-only -I "$CORE" "$f"; then
		echo "PASS $rel compiles alone"
	else
		echo "FAIL $rel does not compile alone"
		rc=1
	fi

	# (b) the file names no vendor and includes nothing foreign
	if out=$(isolation_check "$f"); then
		echo "PASS $rel is neutral"
	else
		echo "FAIL $rel is not neutral"
		echo "$out"
		rc=1
	fi
done

# ---- the control: the checker must still catch the two mutants ----
mutant_src=$(find "$CORE" -type f -name '*.h' | LC_ALL=C sort | head -n 1)
if [ -z "$mutant_src" ]; then
	echo "FAIL no header under sys/core to mutate"
	exit 1
fi

cp "$mutant_src" "$T/mutant_include.h"
printf '\n#include "../nvidia_uvm_rm.h"\n' >> "$T/mutant_include.h"
if out=$(isolation_check "$T/mutant_include.h"); then
	echo "FAIL the mutant with a foreign include was not flagged"
	rc=1
else
	case "$out" in
	*includes*nvidia_uvm_rm.h*)
		echo "PASS the mutant with a foreign include is flagged"
		;;
	*)
		echo "FAIL the mutant with a foreign include was flagged for the wrong reason"
		echo "$out"
		rc=1
		;;
	esac
fi

cp "$mutant_src" "$T/mutant_word.h"
printf '\n/* the word nvidia must not appear here */\n' >> "$T/mutant_word.h"
if out=$(isolation_check "$T/mutant_word.h"); then
	echo "FAIL the mutant with a vendor word was not flagged"
	rc=1
else
	case "$out" in
	*forbidden\ word*)
		echo "PASS the mutant with a vendor word is flagged"
		;;
	*)
		echo "FAIL the mutant with a vendor word was flagged for the wrong reason"
		echo "$out"
		rc=1
		;;
	esac
fi

exit $rc
