#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# gen-uvm-request-table.sh --write|--check [NVIDIA_UVM_IOCTL_H]
#
# Writes linux/uvm_linux_request_table.h: the ONE table that maps the
# driver's request numbers onto the Linux request numbers (Track #2444).
#
# The list of requests comes from NVIDIA's MIT uvm_ioctl.h, which
# tools/gen-ioctl-table.sh (SkunkWerks, BSD-2-Clause) reads: it prints one
# entry per request.  Each entry is then resolved against this repository:
# the driver's own request id (the UVM_REQ_* enumeration of
# sys/nvidia_uvm_ioctl.h), the Linux request number (the LINUX_UVM_*
# constants of linux/nvidia_uvm_linux_ioctl.h) and the parameter structure
# the driver copies for the request.
#
# Only the NAMES of the requests leave NVIDIA's header: no NVIDIA text
# enters this tree.
#
#   --write   write the table
#   --check   write it to a scratch file and compare it with the committed
#             one; a difference is an error
#
# With no header, --check does nothing: the committed table is used as it
# is, so that a build needs no NVIDIA source.

set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MODE=${1:-}
HDR=${2:-${NVIDIA_UVM_IOCTL_H:-}}
TABLE="$REPO/linux/uvm_linux_request_table.h"
IOCTLH="$REPO/linux/nvidia_uvm_linux_ioctl.h"
ABI="$REPO/sys/nvidia_uvm_ioctl.h"

case "$MODE" in
--write|--check) ;;
*)
	printf 'usage: %s --write|--check [path/to/uvm_ioctl.h]\n' "$0" >&2
	exit 2
	;;
esac

if [ -z "$HDR" ]; then
	if [ "$MODE" = --write ]; then
		printf '%s: no NVIDIA uvm_ioctl.h to read\n' "$0" >&2
		exit 2
	fi
	printf '%s: no NVIDIA uvm_ioctl.h: the committed table is used as it is\n' "$0"
	exit 0
fi
[ -f "$HDR" ] || {
	printf '%s: %s is not a file\n' "$0" "$HDR" >&2
	exit 2
}

WORKDIR=${TMPDIR:-${GB10_WORK:-$HOME/work/gb10}/tmp}
mkdir -p "$WORKDIR"
WORK=$(mktemp -d "$WORKDIR/uvm-table.XXXXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT QUIT TERM

has() {
	# has <set> <name>
	case " $1 " in
	*" $2 "*) return 0 ;;
	esac
	return 1
}

# The requests NVIDIA's header lists, in the order of its own ioctl numbers.
# The generator prints one entry per request; only the name is kept.  The
# names are kept in one space-separated list, because that is how they are
# looked up below.
nvidia_names=$(sh "$REPO/tools/gen-ioctl-table.sh" "$HDR" |
    sed -n 's/^[A-Z_][A-Z_0-9]*(\([A-Z_][A-Z_0-9]*\))$/\1/p' | tr '\n' ' ')
[ -n "$nvidia_names" ] || {
	printf '%s: %s lists no request\n' "$0" "$HDR" >&2
	exit 2
}

# The requests of this repository: the Linux request numbers it carries and
# the driver's own request ids.  A request is in the table when both are
# there; a Linux number alone is not a request (IS_8_SUPPORTED, which
# NVIDIA defines and this driver does not serve).
our_names=$(sed -n 's/^#define[[:space:]]*LINUX_UVM_\([A-Z_][A-Z_0-9]*\)[[:space:]].*/\1/p' "$IOCTLH" | tr '\n' ' ')
our_ids=$(sed -n 's/^[[:space:]]*UVM_REQ_\([A-Z_][A-Z_0-9]*\)[[:space:]]*[,=].*/\1/p' "$ABI" | tr '\n' ' ')

# The suffixes of the requests NVIDIA lists: UVM_FOO becomes FOO, which is
# the tail of UVM_REQ_FOO, of LINUX_UVM_FOO and of UVM_FOO_PARAMS.
nvidia_suffixes=""
for n in $nvidia_names; do
	case "$n" in
	UVM_*) n=${n#UVM_} ;;
	esac
	nvidia_suffixes="$nvidia_suffixes $n"
done

# The requests of this driver that NVIDIA's header does not list: the two
# initialisation requests, which NVIDIA's Linux interface does not have.
# They come first, in the order of the driver's own request ids.
ours=""
for s in $our_names; do
	has "$our_ids" "$s" || continue
	has "$nvidia_suffixes" "$s" && continue
	ours="$ours $s"
done

# The requests NVIDIA's header lists, in its order: a request of this
# driver whose Linux number this repository carries.
theirs=""
for s in $nvidia_suffixes; do
	has "$our_ids" "$s" || continue
	has "$our_names" "$s" || continue
	theirs="$theirs $s"
done

# The Linux request numbers this repository carries must be the ones
# NVIDIA's header carries: the table is worth nothing if the two disagree.
awk '
/^#define[ \t]+UVM_[A-Z0-9_]+[ \t]+UVM_IOCTL_BASE\([0-9]+\)/ {
	name = $2; n = $3
	sub(/^UVM_IOCTL_BASE\(/, "", n); sub(/\)$/, "", n)
	printf "%s %s\n", name, n
}' "$HDR" >"$WORK/nvidia_numbers"

bad=0
while read -r name number; do
	suffix=${name#UVM_}
	has "$our_names" "$suffix" || continue
	value=$(sed -n "s/^#define[[:space:]]*LINUX_UVM_${suffix}[[:space:]]*//p" "$IOCTLH" |
	    sed 's/[uU]$//')
	case "$value" in
	'' | *[!0-9]*)
		# not a plain decimal number: not comparable (the two
		# initialisation requests, which NVIDIA's header does not list)
		continue
		;;
	esac
	if [ "$value" != "$number" ]; then
		printf '%s: the Linux number of %s is %s, NVIDIA says %s\n' \
		    "$0" "$suffix" "$value" "$number" >&2
		bad=1
	fi
done <"$WORK/nvidia_numbers"
[ "$bad" = 0 ] || {
	printf '%s: the Linux request numbers do not match NVIDIA'"'"'s header\n' "$0" >&2
	exit 1
}

# The requests that take a parameter structure: the ABI header names it
# after the request (UVM_FOO_PARAMS).  The deinitialisation takes none: it
# is answered before the driver's own lookup.
with_params=""
for s in $ours $theirs; do
	grep -q "UVM_${s}_PARAMS" "$ABI" && with_params="$with_params $s"
done

# The rows, one per request: the name, the driver's request id, the Linux
# request number and the size of the parameter structure.  A request that
# takes no parameter structure is written with X0.
printf '%s\n' $ours $theirs | awk -v with=" $with_params " '
{
	suffix = $0
	if (suffix == "")
		next
	kind = (index(with, " " suffix " ") > 0) ? "X" : "X0"
	printf "\t%s(%s)", kind, suffix
	for (i = length(suffix); i < 33; i++)
		printf " "
	printf "\\\n"
}' >"$WORK/rows"

{
	cat <<'EOF'
/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_linux_request_table.h - the one table that maps the driver's request
 * numbers onto the Linux request numbers (Track #2444).
 *
 * GENERATED FILE: do not edit it.  tools/gen-uvm-request-table.sh writes
 * it: the list of requests comes from NVIDIA's MIT uvm_ioctl.h, read by
 * tools/gen-ioctl-table.sh (SkunkWerks, BSD-2-Clause), and every request
 * is resolved against this repository - the driver's own request id (the
 * UVM_REQ_* enumeration of sys/nvidia_uvm_ioctl.h), the Linux request
 * number (LINUX_UVM_*) and the parameter structure the driver copies for
 * the request.  Only the names of the requests leave NVIDIA's header.
 *
 * Regenerate it with:
 *     make regen-ioctl-table NVIDIA_UVM_IOCTL_H=/path/to/uvm_ioctl.h
 *
 * X(name) is a request that takes a parameter structure and X0(name) the
 * one that takes none: the deinitialisation, which the driver answers
 * before its own lookup.
 */
#ifndef UVM_LINUX_REQUEST_TABLE_H
#define UVM_LINUX_REQUEST_TABLE_H

#define	UVM_LINUX_REQUEST_TABLE(X, X0)					\
EOF
	cat "$WORK/rows"
	cat <<'EOF'

#endif /* UVM_LINUX_REQUEST_TABLE_H */
EOF
} >"$WORK/table.h"

case "$MODE" in
--write)
	cp "$WORK/table.h" "$TABLE"
	printf '%s: wrote %s\n' "$0" "$TABLE"
	;;
--check)
	if cmp -s "$WORK/table.h" "$TABLE"; then
		printf '%s: the committed table matches NVIDIA'"'"'s header\n' "$0"
	else
		printf '%s: the committed table does not match NVIDIA'"'"'s header;\n' "$0" >&2
		printf '    regenerate it with: make regen-ioctl-table NVIDIA_UVM_IOCTL_H=%s\n' "$HDR" >&2
		exit 1
	fi
	;;
esac
