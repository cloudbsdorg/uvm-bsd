#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# bare-shutdown-check.sh - the one detector for a bare shutdown call in a kit's rc script (dot-sourced by check-be.sh
# and by the host test; it defines a function and runs nothing).
#
#   bare_shutdown_lines FILE   prints the lines of FILE that use shutdown outside the guarded call; empty output = clean
#
# What is removed before the search (and only that):
#   - comment lines;
#   - the <kit>_real_shutdown=/sbin/shutdown assignment line;
#   - the call itself on the one line that has it ("$<kit>_shutdown" -r "+N" "wall message"); the rest of that line
#     (a redirect, or anything appended after it) is still searched;
#   - the quoted arguments of row, echo and printf lines (messages).
# Everything else, quoted or not, is searched.  A hit is the token shutdown as a command word (at the start of a line,
# or after if/then/else/elif/do/while/until/!/{/(/;/&/|/backtick, exec, command, nohup, time, eval, env, xargs, sudo,
# doas, or the -c of sh -c and bash -c, with an optional opening quote), or followed by a flag (-r, -h, -p, -k, -o,
# now, +N), or the literal path /sbin/shutdown anywhere else.

bare_shutdown_lines()
{
	grep -v '^[[:space:]]*#' "$1" \
	| grep -v '^[A-Za-z_][A-Za-z0-9_]*_real_shutdown=/sbin/shutdown$' \
	| sed -E -e 's/^[[:space:]]*"\$[A-Za-z_][A-Za-z0-9_]*_shutdown" -r "[^"]*" "[^"]*"//' \
	    -e '/^[[:space:]]*(row|echo|printf)[[:space:]]/{s/"[^"]*"//g;s/'"'"'[^'"'"']*'"'"'//g;}' \
	| grep -E '(^|[;&|({`!]|(^|[[:space:]])(if|then|else|elif|do|while|until|exec|command|nohup|time|eval|env|xargs|sudo|doas|-c)[[:space:]])[[:space:]]*["'"'"']?(/sbin/)?shutdown([^A-Za-z0-9_-]|$)|(^|[^A-Za-z0-9_$.-])shutdown[[:space:]]+(-[rhpko]|now|\+[0-9])|/sbin/shutdown'
	return 0
}
