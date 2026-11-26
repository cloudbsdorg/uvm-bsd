#!/bin/sh
# Copyright (c) 2026 SkunkWerks, GmbH
# SPDX-License-Identifier: BSD-2-Clause
#
# gen-ioctl-table.sh UVM_IOCTL_H > uvm_ioctl_table.h
#
# Reads NVIDIA's uvm_ioctl.h (MIT) and prints one table entry per ioctl:
#   UVM_E(NAME)           the request has a NAME_PARAMS struct with an rmStatus member
#   UVM_E_NOSTATUS(NAME)  it has a NAME_PARAMS struct without rmStatus
#   UVM_E_NOPARAMS(NAME)  it has no NAME_PARAMS struct
# Sizes and offsets are taken by the compiler from the header itself, so no
# NVIDIA text is copied into this repository.
set -eu
[ $# -eq 1 ] && [ -f "$1" ] || { echo "usage: $0 uvm_ioctl.h" >&2; exit 2; }
awk '
# leave out what the header gives to Windows only
/^#if/                           { depth++; if ($0 ~ /WIN32/ && !skip) skip = depth; next }
/^#else/                         { if (skip == depth) skip = 0; next }
/^#endif/                        { if (skip == depth) skip = 0; depth--; next }
skip                             { next }
/^typedef struct/               { in_struct = 1; has_status = 0; next }
in_struct && /rmStatus/          { has_status = 1 }
in_struct && /^}[ \t]*[A-Z0-9_]+_PARAMS;/ {
	name = $2; sub(/;$/, "", name); sub(/_PARAMS$/, "", name)
	params[name] = 1; status[name] = has_status; in_struct = 0; next
}
/^#define[ \t]+UVM_[A-Z0-9_]+[ \t]+UVM_IOCTL_BASE\([0-9]+\)/ { order[++n] = $2 }
END {
	for (i = 1; i <= n; i++) {
		name = order[i]
		if (!(name in params))      printf "UVM_E_NOPARAMS(%s)\n", name
		else if (status[name])      printf "UVM_E(%s)\n", name
		else                        printf "UVM_E_NOSTATUS(%s)\n", name
	}
}' "$1"
