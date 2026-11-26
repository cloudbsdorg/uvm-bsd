# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# pins_lib.sh - helpers for source-level pin tests (sourced, not run).
# A pin looks at comment-stripped source, so a commented-out call does not
# count, and every pin is tested against planted defects (testing-standard
# rule 6): the pin must pass on the real tree and fail on each mutation.

# strip C comments (block and line), keeping line numbers
pin_strip() {
	awk '
	{ line = $0; out = ""
	  while (length(line) > 0) {
		if (inc) { i = index(line, "*/"); if (i == 0) { line = ""; break } line = substr(line, i + 2); inc = 0; continue }
		i = index(line, "/*"); j = index(line, "//")
		if (j > 0 && (i == 0 || j < i)) { out = out substr(line, 1, j - 1); line = ""; break }
		if (i == 0) { out = out line; line = ""; break }
		out = out substr(line, 1, i - 1); line = substr(line, i + 2); inc = 1
	  }
	  print out }' "$1"
}

# pin_fn FILE NAME: the comment-stripped body of function NAME (definition at column 0)
pin_fn() {
	pin_strip "$1" | awk -v fn="$2" '$0 ~ "^" fn "\\(" { f = 1 } f { print } f && /^}/ { exit }'
}

# pin_has TEXT REGEX: the text has a line matching the (extended) regex
pin_has() { printf '%s\n' "$1" | grep -Eq -- "$2"; }

# pin_dead TEXT: the text has no disabled-code idiom
pin_dead() {
	! printf '%s\n' "$1" | grep -Eq -- 'if \((0|1|false|true)\b|\(0 &&|\|\| 1\b|^#[[:space:]]*if[[:space:]]+[01]\b'
}

# the extra source directories a pin test copies into its scratch directory
# (the optional Linux ioctl bridge module lives outside sys/): a space
# separated list of directories whose *.c and *.h are copied next to the
# files of the system directory.
PIN_EXTRA_DIRS=

# mutation self-test: pin_mutate NAME PIN_FN FILE SED_EXPR  (PIN_FN takes the sys directory)
#   copies $SYS to a scratch directory, applies the sed expression to FILE
#   there, and requires the pin function to fail.
pin_mutate() {
	_name=$1; _fn=$2; _file=$3; _expr=$4
	_d=$PIN_TMP/m.$$.$PIN_N; PIN_N=$((PIN_N + 1))
	mkdir -p "$_d" && cp "$SYS"/*.c "$SYS"/*.h "$_d"/
	for _x in $PIN_EXTRA_DIRS; do
		cp "$_x"/*.c "$_x"/*.h "$_d"/ 2>/dev/null || true
	done
	if ! sed -e "$_expr" "$_d/$_file" > "$_d/$_file.new" || cmp -s "$_d/$_file" "$_d/$_file.new"; then
		echo "FAIL self-test $_name: the mutation changed nothing"; rc=1; return
	fi
	mv "$_d/$_file.new" "$_d/$_file"
	if "$_fn" "$_d" >/dev/null 2>&1; then
		echo "FAIL self-test $_name: planted defect not caught"; rc=1
	fi
	rm -rf "$_d"
}
