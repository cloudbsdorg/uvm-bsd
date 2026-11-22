#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# summarize.sh - read the results of one nvidia_client_owner_testboot run and say what they mean, by the
# reading rule of the merge review (Track #2373):
#   - the verdict of run_owner_boot.sh is its last line: PASS is GO evidence, FAIL is NO-GO, RE-RUN is neither
#     (a vacuous loop, or another process holding /dev/nvidiactl);
#   - a staged module that fails to load, a step left "running" (a panic or a kill) or a run with no final "done"
#     row is NO-GO; a kit PRECONDITION that failed (module hash, nvidia-drm staged, test uids, dead-man timer,
#     panic settings, an nvidia already loaded, linux64, the GSP firmware module not loaded, the GPU node missing) measured nothing about the change: it is KIT (exit 4);
#   - the host's validation setting must have been READ (RmValidateClientData absent or harmless): a PASS
#     without that line is neither;
#   - loop 5 counts as evidence for the tombstone path only when iterations_b is above 0; even then race.err
#     is read by hand (open item loop5-contact-soundness);
#   - this boot cannot show close-path (no loop closes the owner of a converted record): the model row and the
#     glue scenario are its evidence.
#
# usage: summarize.sh <run directory>   (the directory holding results.jsonl and work/logs/uvm-2373-boot/<stamp>/)
# Exit codes: 0 GO, 1 NO-GO, 2 usage or unreadable, 3 neither (RE-RUN, or a PASS that proves less than it says),
# 4 KIT (the test could not be run: fix the kit or the host and run again).
# The last line printed is GO, NO-GO, NEITHER or KIT.

set -u
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
export PATH
[ $# -eq 1 ] && [ -d "$1" ] || { echo "usage: summarize.sh <run directory>" >&2; exit 2; }
RUN=$1
ROWS=$RUN/results.jsonl
[ -r "$ROWS" ] || { echo "summarize: cannot read $ROWS" >&2; exit 2; }

PRE='^(hash-|no-drm|load-skipped-no-deadman|test-uids|deadman|panic-settings|preloaded|load-linux64|load-nvidia_gsp_ga10x_fw|firmware-loaded|gpu-node|load$)'
# the failed precondition rows, by name
prenames=$(grep '"result":"fail"' "$ROWS" | sed 's/.*"row":"\([^"]*\)".*/\1/' | grep -E "$PRE" | tr '\n' ' ')
bename=$(sed -n 's/.*"row":"kit-version".*BE \([A-Za-z0-9._-]*\),.*/\1/p' "$ROWS" | head -1)
again="to run again: build a new boot environment with make-be.sh, or re-arm ${bename:-the one that ran} (bectl activate -t ${bename:-BE}, from the normal boot environment)"
bad=0
kit=0
awk -v prere="$PRE" '
function field(line, name,   p, rest) {
	p = index(line, "\"" name "\":\"")
	if (p == 0) return ""
	rest = substr(line, p + length(name) + 4)
	return substr(rest, 1, index(rest, "\"") - 1)
}
{
	r = field($0, "row"); res = field($0, "result")
	if (r == "") next
	n++
	if (!(r in seen)) { seen[r] = 1; order[++k] = r }
	last[r] = res
	if (res == "fail") anyfail[r] = 1
	if (res == "fail" && r ~ prere) pre[r] = 1
}
END {
	bad = 0; kit = 0
	if (n == 0) { print "FAIL: no kit rows"; exit 1 }
	for (i = 1; i <= k; i++) {
		r = order[i]
		printf "%-6s %s\n", (r in anyfail) ? "fail" : last[r], r
		if (r in pre) { kit++; continue }
		if (r in anyfail) bad++
		if (last[r] == "running") { printf "FAIL: the run stopped in step \"%s\" (a panic or a kill)\n", r; bad++ }
	}
	if (!("done" in seen) || last["done"] != "info") { print "FAIL: no final \"done\" row: the run did not finish"; bad++ }
	if (bad) exit 1
	exit (kit ? 4 : 0)
}' "$ROWS"
case $? in 0) ;; 4) kit=1 ;; *) bad=1 ;; esac

# the owner-boot result of this run: exactly one directory
set -- "$RUN"/work/logs/uvm-2373-boot/*/result.txt
[ -e "$1" ] || set --
if [ $# -ne 1 ] || [ ! -r "$1" ]; then
	echo "no single run_owner_boot result under $RUN/work/logs/uvm-2373-boot (found $#): read rc.log and owner-boot.out in $RUN"
	if [ "$kit" = 0 ]; then bad=1; fi
	RES=
else
	RES=$1
fi
verdict=
if [ -n "$RES" ]; then
	verdict=$(tail -1 "$RES")
	echo "run_owner_boot.sh verdict: $verdict ($RES)"
fi
if [ "$bad" != 0 ]; then
	echo "summary: NO-GO (a staged module did not load, the run stopped in a step or did not finish, or its result is missing)"
	echo NO-GO
	exit 1
fi
# a measured FAIL is never hidden by a failed precondition: FAIL wins
if [ "$kit" != 0 ] && [ "$verdict" = FAIL ]; then
	echo "summary: NO-GO (run_owner_boot.sh says FAIL: read $RES); precondition rows also failed: $prenames: the FAIL stands, but fix those before trusting any re-run; $again"
	echo NO-GO
	exit 1
fi
if [ "$kit" != 0 ]; then
	echo "summary: KIT: a precondition of the test failed: $prenames: nothing was measured about the change; fix it; $again"
	echo KIT
	exit 4
fi
case $verdict in
FAIL)
	echo "summary: NO-GO (run_owner_boot.sh says FAIL: read $RES)"
	echo NO-GO
	exit 1 ;;
RE-RUN)
	echo "summary: neither GO nor NO-GO: run_owner_boot.sh says RE-RUN (a vacuous loop, another holder of /dev/nvidiactl, or an unread validation setting): read $RES; $again"
	echo NEITHER
	exit 3 ;;
PASS) ;;
*)
	echo "summary: NO-GO (the last line of $RES is not PASS, FAIL or RE-RUN: \"$verdict\")"
	echo NO-GO
	exit 1 ;;
esac
# PASS: the validation setting must have been read
if ! grep -Eq '^ok   (RmValidateClientData does not turn validation off|no RmValidateClientData in kenv or sysctl)' "$RES"; then
	echo "summary: neither GO nor NO-GO: PASS, but no line says the host's validation setting was read (RmValidateClientData): the refusals prove nothing without it; read $RES"
	echo NEITHER
	exit 3
fi
D=$(dirname "$RES")
ib=$(sed -n 's/.*"loop":5,.*"iterations_b":\([0-9]*\),.*/\1/p' "$D/race.jsonl" 2>/dev/null | head -1)
if [ -z "$ib" ] || [ "$ib" = 0 ]; then
	echo "summary: neither GO nor NO-GO: PASS, but loop 5 did no work against a client (iterations_b '${ib:-missing}'): the tombstone path was not exercised"
	echo NEITHER
	exit 3
fi
echo "note: loop 5 iterations_b $ib: evidence for the tombstone path; read $D/race.err by hand (open item loop5-contact-soundness)"
echo "note: this boot cannot show close-path (no loop closes the owner of a converted record); its evidence is the model row and the glue scenario"
echo "summary: GO"
echo GO
exit 0
