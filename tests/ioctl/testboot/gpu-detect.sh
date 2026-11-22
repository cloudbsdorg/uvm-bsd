#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# gpu-detect.sh - find the NVIDIA GPU node of this host by what the machine reports, never by a fixed index
# (skill hardware-preflight-and-verify, whose JSON shape this follows; Track #2373).  On amp001 pciconf -l lists
# vgapci0 (AMD, pci12:3:0:0), vgapci1 (NVIDIA 0x10de:0x2d04, pci13:1:0:0) and vgapci2 (the ASPEED BMC): pciconf names
# an NVIDIA card vgapciN, the nvidia driver's own unit follows its vgapci parent, so the node is /dev/nvidia1 there; other
# machines number differently again.
#
# Usage: gpu-detect.sh [-D DEVDIR] [-d NODE]
#   -D DEVDIR  where the device nodes are (default /dev)
#   -d NODE    override: use this node (its full path, as devices[].node prints it) instead of the rule; it must be a usable
#              NVIDIA node found
# Output: one JSON object on standard output (version 1, schema gpu-detect/1): status, the rule, the selected node
# and its PCI identity, every NVIDIA display device found (devices, with its node, or the reason it has none), every
# other display device (excluded: BMC, AMD, Intel, ...), and errors.  Exit 0 for selected or none (an empty host is a
# legitimate result), 2 for a usage error, an override that names no usable node, or input that cannot be read
# (status error: a pciconf line of display class that does not parse is an error, never a silent drop).
#
# How each card is found:
#   1. every line of pciconf -l of class 0x03xxxx (0x0300 VGA, 0x0302 3D/compute, 0x0380 other display) is a display
#      device, identified by its PCI address (pciD:B:S:F as in dev.vgapci.N.%location dbsf=); the BMC vendor 0x1a03 and
#      every vendor but NVIDIA (0x10de) go to excluded;
#   2. every /dev/nvidiaN node (digits only: nvidiactl, nvidia-modeset and nvidia-uvm are not GPUs) is mapped to its PCI
#      device through the driver: dev.nvidia.N.%parent names the vgapci device, never the order of the nodes; the
#      address in dev.vgapci.M.%location must equal the pciconf address, or the card is not usable;
#   3. the rule: compute controllers (class 0x0302) before display controllers, then the lowest PCI address;
#   4. the device ID selects the family (it is in the output); the subsystem ID is for decorators and is not used here.
#
# The identity check after a node is opened is made by the caller, which asks the driver about the opened node (its unit's
# parent, that parent's dbsf address, and pciconf for its vendor and device) and compares with the choice made here.

set -u
PATH=$PATH:/sbin:/usr/sbin:/bin:/usr/bin:/usr/local/bin
LC_ALL=C
export PATH LC_ALL
DEVDIR=/dev
WANT=
usage() { sed -n '5,/^# How each card/p' "$0" | sed '$d; s/^# \{0,1\}//' >&2; exit 2; }
while [ $# -gt 0 ]; do
	case $1 in
	-D) [ $# -ge 2 ] || usage; DEVDIR=$2; shift 2 ;;
	-d) [ $# -ge 2 ] || usage; WANT=$2; shift 2 ;;
	-h | --help) usage ;;
	*) echo "gpu-detect.sh: unknown argument: $1" >&2; usage ;;
	esac
done
RULE="compute controllers (class 0x0302) before display controllers (class 0x0300, 0x0380), then the lowest PCI address"
NL='
'

json_str() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr -c '[:print:]' ' '; }
# field <line> <name>: the 0x value of "name=" in a pciconf -l line
field() { printf '%s\n' "$1" | sed -n "s/.*[[:space:]]$2=\\(0x[0-9a-fA-F]*\\).*/\\1/p" | head -1 | tr 'A-F' 'a-f'; }
# dbsf <line>: pciD:B:S:F of a pciconf -l line
dbsf() { printf '%s\n' "$1" | sed -n 's/^[^@]*@\(pci[0-9]*:[0-9]*:[0-9]*:[0-9]*\):.*/\1/p'; }
# sortkey <dbsf>: zero-padded so a plain sort orders by domain, bus, slot, function
sortkey() { printf '%s\n' "$1" | sed 's/^pci//' | awk -F: '{ printf "%06d%04d%04d%04d", $1, $2, $3, $4 }'; }
role_of() {
	case $2 in 0x1a03) echo bmc; return ;; esac
	case $1 in 0x0302*) echo compute ;; *) echo display ;; esac
}
cutf() { printf '%s' "$1" | cut -d'|' -f"$2"; }

errors=
add_error() { errors="$errors$NL$1"; }

# 1. the display devices of pciconf -l
pcilist=$(pciconf -l 2> /dev/null) || { add_error "pciconf -l failed"; pcilist=; }
[ -n "$pcilist" ] || add_error "pciconf -l printed nothing"
devs=	# sortkey|name|dbsf|vendor|device|class|role
OLDIFS=$IFS
IFS=$NL
for line in $pcilist; do
	case $line in *class=0x03*) ;; *) continue ;; esac
	name=${line%%@*}; bus=$(dbsf "$line")
	class=$(field "$line" class); vendor=$(field "$line" vendor); device=$(field "$line" device)
	if [ -z "$bus" ] || [ -z "$class" ] || [ -z "$vendor" ] || [ -z "$device" ] || [ -z "$name" ]; then
		add_error "a display-class line of pciconf -l does not parse: $(printf '%s' "$line" | tr -c '[:print:]' ' ')"
		continue
	fi
	devs="$devs$NL$(sortkey "$bus")|$name|$bus|$vendor|$device|$class|$(role_of "$class" "$vendor")"
done
IFS=$OLDIFS
devs=$(printf '%s\n' "$devs" | sed '/^$/d' | sort)

# 2. the nodes, mapped through dev.nvidia.N.%parent
units=
for p in "$DEVDIR"/nvidia[0-9]*; do
	[ -e "$p" ] || continue
	n=${p##*/}
	case ${n#nvidia} in '' | *[!0-9]*) continue ;; esac
	units="$units ${n#nvidia}"
done
units=$(printf '%s\n' $units | sort -n | tr '\n' ' ')
nodemap=	# parent|node|unit   (parent is - when the driver reports none)
for u in $units; do
	par=$(sysctl -n "dev.nvidia.$u.%parent" 2> /dev/null | head -1)
	nodemap="$nodemap$NL${par:--}|$DEVDIR/nvidia$u|$u"
done
nodemap=$(printf '%s\n' "$nodemap" | sed '/^$/d')

# the NVIDIA devices with their node, usability and reason; every other display device is excluded
rows=	# prio|sortkey|name|dbsf|vendor|device|class|role|node|unit|usable|reason
excl=	# dbsf|name|vendor|device|class|role|reason
orphans=	# node|unit|reason
IFS=$NL
for d in $devs; do
	name=$(cutf "$d" 2); bus=$(cutf "$d" 3); vendor=$(cutf "$d" 4); device=$(cutf "$d" 5); class=$(cutf "$d" 6); role=$(cutf "$d" 7)
	if [ "$vendor" != 0x10de ]; then
		case $vendor in
		0x1a03) why="ASPEED BMC video chip, not a compute GPU" ;;
		0x1002) why="AMD display device, not NVIDIA" ;;
		0x8086) why="Intel display device, not NVIDIA" ;;
		*) why="vendor $vendor is not NVIDIA" ;;
		esac
		excl="$excl$NL$bus|$name|$vendor|$device|$class|$role|$why"
		continue
	fi
	node=; unit=; usable=0; why="no /dev/nvidiaN node: no nvidia unit has $name as its parent"
	hit=$(printf '%s\n' "$nodemap" | awk -F'|' -v p="$name" '$1 == p { print; exit }')
	if [ -n "$hit" ]; then
		node=$(cutf "$hit" 2); unit=$(cutf "$hit" 3)
		pu=${name#vgapci}
		loc=$(sysctl -n "dev.vgapci.$pu.%location" 2> /dev/null | sed -n 's/.*dbsf=\(pci[0-9]*:[0-9]*:[0-9]*:[0-9]*\).*/\1/p' | head -1)
		if [ -z "$loc" ]; then
			why="dev.vgapci.$pu.%location gives no address (dbsf=): the driver does not confirm where $name is"
		elif [ "$loc" != "$bus" ]; then
			why="dev.vgapci.$pu.%location says $loc but pciconf says $bus"
		else
			usable=1; why=
		fi
	fi
	case $role in compute) prio=0 ;; *) prio=1 ;; esac
	[ "$usable" = 1 ] || prio=9
	rows="$rows$NL$prio|$(cutf "$d" 1)|$name|$bus|$vendor|$device|$class|$role|$node|$unit|$usable|$why"
done
# nodes no NVIDIA display device claims
for n in $nodemap; do
	par=$(cutf "$n" 1)
	if [ "$par" = - ]; then
		orphans="$orphans$NL$(cutf "$n" 2)|$(cutf "$n" 3)|the driver reports no parent for dev.nvidia.$(cutf "$n" 3).%parent"
	elif ! printf '%s\n' "$rows" | awk -F'|' -v p="$par" '$3 == p { f = 1 } END { exit f ? 0 : 1 }'; then
		orphans="$orphans$NL$(cutf "$n" 2)|$(cutf "$n" 3)|its parent $par is not an NVIDIA display device of pciconf -l"
	fi
done
IFS=$OLDIFS
rows=$(printf '%s\n' "$rows" | sed '/^$/d' | sort -t'|' -k1,1n -k2,2)

# the choice
status=none
override=false
selline=
if [ -n "$WANT" ]; then
	override=true
	selline=$(printf '%s\n' "$rows" | awk -F'|' -v n="$WANT" '$9 == n && $11 == 1 { print; exit }')
	if [ -n "$selline" ]; then status=selected; else status=override-not-found; fi
else
	selline=$(printf '%s\n' "$rows" | awk -F'|' '$11 == 1 { print; exit }')
	[ -z "$selline" ] || status=selected
fi
if [ -n "$errors" ] && [ "$status" != override-not-found ]; then status=error; fi
selnode=; [ -z "$selline" ] || selnode=$(cutf "$selline" 9)
nvrows=$(printf '%s\n' "$rows" | sed '/^$/d' | wc -l | tr -d ' ')
case $status in
selected) detail="selected $selnode" ;;
override-not-found) detail="the override $WANT is not a usable NVIDIA node found" ;;
error) detail="pciconf -l could not be read completely: see errors" ;;
*) if [ "$nvrows" -gt 0 ]; then detail="$nvrows NVIDIA display device(s) present but none usable: see devices (reason)"; else detail="no NVIDIA display device in pciconf -l"; fi ;;
esac

# output
printf '{\n'
printf '  "version": 1,\n'
printf '  "schema": "gpu-detect/1",\n'
printf '  "status": "%s",\n' "$status"
printf '  "rule": "%s",\n' "$(json_str "$RULE")"
printf '  "override": %s,\n' "$override"
if [ -n "$selline" ] && [ "$status" != error ]; then
	printf '  "selected_node": "%s",\n' "$(json_str "$selnode")"
	printf '  "selected_bus_id": "%s",\n' "$(cutf "$selline" 4)"
	printf '  "selected_vendor": "%s",\n' "$(cutf "$selline" 5)"
	printf '  "selected_device": "%s",\n' "$(cutf "$selline" 6)"
else
	printf '  "selected_node": null,\n'
fi
printf '  "devices": ['
first=1; i=0
IFS=$NL
for r in $rows; do
	[ "$first" = 1 ] || printf ','
	first=0
	node=$(cutf "$r" 9); unit=$(cutf "$r" 10); issel=false
	[ -z "$node" ] || [ "$node" != "$selnode" ] || [ "$status" = error ] || issel=true
	[ -n "$node" ] && nodej="\"$(json_str "$node")\"" || nodej=null
	[ -n "$unit" ] && unitj=$unit || unitj=null
	[ "$(cutf "$r" 11)" = 1 ] && usable=true || usable=false
	printf '\n    {"index": %s, "name": "%s", "bus": "%s", "vendor": "%s", "device": "%s", "class": "%s", "role": "%s", "node": %s, "unit": %s, "usable": %s, "selected": %s, "reason": "%s"}' \
	    "$i" "$(cutf "$r" 3)" "$(cutf "$r" 4)" "$(cutf "$r" 5)" "$(cutf "$r" 6)" "$(cutf "$r" 7)" "$(cutf "$r" 8)" \
	    "$nodej" "$unitj" "$usable" "$issel" "$(json_str "$(cutf "$r" 12)")"
	i=$((i + 1))
done
for o in $orphans; do
	[ "$first" = 1 ] || printf ','
	first=0
	printf '\n    {"index": %s, "name": null, "bus": null, "vendor": null, "device": null, "class": null, "role": null, "node": "%s", "unit": %s, "usable": false, "selected": false, "reason": "%s"}' \
	    "$i" "$(json_str "$(cutf "$o" 1)")" "$(cutf "$o" 2)" "$(json_str "$(cutf "$o" 3)")"
	i=$((i + 1))
done
printf '\n  ],\n'
printf '  "excluded": ['
first=1
for r in $excl; do
	[ "$first" = 1 ] || printf ','
	first=0
	printf '\n    {"pci": "%s", "name": "%s", "vendor": "%s", "device": "%s", "class": "%s", "role": "%s", "reason": "%s"}' \
	    "$(cutf "$r" 1)" "$(cutf "$r" 2)" "$(cutf "$r" 3)" "$(cutf "$r" 4)" "$(cutf "$r" 5)" "$(cutf "$r" 6)" "$(json_str "$(cutf "$r" 7)")"
done
printf '\n  ],\n'
printf '  "errors": ['
first=1
for r in $errors; do
	[ "$first" = 1 ] || printf ','
	first=0
	printf '\n    "%s"' "$(printf '%s' "$r" | sed 's/\\/\\\\/g; s/"/\\"/g')"
done
IFS=$OLDIFS
printf '\n  ],\n'
printf '  "detail": "%s"\n}\n' "$(json_str "$detail")"
case $status in override-not-found | error) exit 2 ;; esac
exit 0
