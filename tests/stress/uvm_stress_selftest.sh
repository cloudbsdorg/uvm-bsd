#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# uvm_stress_selftest.sh - fake-mode tests of tests/stress/uvm_stress.sh
# (Track #2272): no module, no GPU, no root.  Fakes stand in for doas,
# sysctl, fstat (as an unprivileged user sees it: no descriptors), the CUDA
# workers, the churn harness and the kernel log.  Each scenario is one run
# of the script and asserts its exit status and log lines:
#   holder   an unprivileged fstat shows nothing; the holder is still
#            proven by the marker it writes, and the run passes
#   stop     a failure ends the run early; the churn loops must stop and
#            the script must exit by itself, with FAIL
#   warn     "GPU lookup by UUID failed" lines already in the log are
#            baseline; new ones are a WARN, not a FAIL
#   nowarn   only baseline lines: no WARN at all
# Run: sh tests/stress/uvm_stress_selftest.sh   Exit 1 on any failure.

set -u
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
export PATH LC_ALL=C
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STRESS=$HERE/uvm_stress.sh
WORK=${GB10_WORK:-$HOME/work/gb10}
mkdir -p "$WORK/tmp"
D=$(mktemp -d "$WORK/tmp/uvm-stress-selftest.XXXXXXXX") || exit 2
trap 'pkill -f "sleep 86400" 2>/dev/null; rm -rf "$D"' EXIT HUP INT TERM
FAILS=0
SCENARIO_FAILS=0

mkdir -p "$D/bin"
cat >"$D/bin/doas" <<'EOT'
#!/bin/sh
case "$1" in
kldunload)
	if pgrep -f 'sleep 86400' >/dev/null; then
		echo "kldunload: can't unload file: Device busy" >&2
		exit 1
	fi
	exit 0 ;;
kldload) exit 0 ;;
esac
exit 1
EOT
cat >"$D/bin/sysctl" <<'EOT'
#!/bin/sh
case "$*" in
*fatal_error*) echo 0 ;;
esac
exit 0
EOT
cat >"$D/bin/fstat" <<'EOT'
#!/bin/sh
# an unprivileged fstat of a process that is P_SUGID: nothing at all
exit 0
EOT
chmod +x "$D/bin/doas" "$D/bin/sysctl" "$D/bin/fstat"
cat >"$D/churn" <<'EOT'
#!/bin/sh
sleep 0.2
EOT
chmod +x "$D/churn"
: >"$D/fakedev"

UUID_LINE='nvidia-uvm: GPU lookup by UUID failed: NV_ERR_GPU_UUID_NOT_FOUND'

# run NAME WORKER-BODY LOG-SEED SECONDS ARGS...: one fake run; prints nothing, sets RC and OUT
run() {
	name=$1; body=$2; seed=$3; secs=$4; shift 4
	printf '%s\n' "$seed" | sed '/^$/d' >"$D/$name.klog"
	printf '#!/bin/sh\n%s\nsleep 1\n' "$body" | sed "s|@LOG@|$D/$name.klog|g" >"$D/$name.worker"
	GB10_WORK=$D/$name.work UVM_STRESS_NO_DEVICE_CHECK=1 UVM_STRESS_DEV=$D/fakedev \
	    UVM_STRESS_PATH_PREFIX=$D/bin UVM_STRESS_KERNLOG=$D/$name.klog \
	    UVM_STRESS_CHURN_CMD=$D/churn UVM_STRESS_SECONDS=$secs UVM_STRESS_PROBE_EVERY=2 \
	    RUN_CUDA=$D/$name.worker UVM_KO=/nonexistent/nvidia-uvm.ko \
	    timeout 90 sh "$STRESS" -j 1 -c 1 "$@" >"$D/$name.out" 2>&1
	RC=$?
	OUT=$D/$name.out
}

expect() {
	if [ "$2" = yes ]; then
		grep -qE -- "$3" "$OUT" || { echo "FAIL $1: output lacks /$3/"; FAILS=$((FAILS + 1)); return; }
	else
		grep -qE -- "$3" "$OUT" && { echo "FAIL $1: output has /$3/"; FAILS=$((FAILS + 1)); return; }
	fi
	return 0
}
expect_rc() {
	[ "$RC" -eq "$3" ] || { echo "FAIL $1: exit status $RC, wanted $3"; FAILS=$((FAILS + 1)); sed 's/^/    /' "$OUT" | tail -15; }
}

scenario_done() {
	if [ "$FAILS" -eq "$SCENARIO_FAILS" ]; then
		echo "ok   scenario $1"
	else
		echo "FAIL scenario $1"
	fi
	SCENARIO_FAILS=$FAILS
}

# holder: fstat is blind; the marker proves the open
run holder 'exit 0' '' 8 -U -H hold
expect_rc holder x 0
expect holder no 'FAIL'
expect holder yes 'with a holder [1-9]'
expect holder yes 'PASS'
scenario_done holder

# stop: a FATAL line ends the run at once; the churn loops must not keep the script alive for 300 s
run stop "echo 'nvidia-uvm: FATAL: boom (NV_ERR_GENERIC)' >>@LOG@" '' 300
expect_rc stop x 1
expect stop yes 'FAIL'
scenario_done stop

# warn: baseline UUID lines are not new; the new ones are a WARN, never a FAIL
run warn "echo '$UUID_LINE' >>@LOG@" "$UUID_LINE
$UUID_LINE" 6
expect_rc warn x 0
expect warn yes 'WARN: [1-9][0-9]* new'
expect warn no 'FAIL'
expect warn yes 'PASS'
scenario_done warn

# nowarn: only baseline lines
run nowarn 'exit 0' "$UUID_LINE
$UUID_LINE" 5
expect_rc nowarn x 0
expect nowarn no 'WARN'
expect nowarn yes 'PASS'
scenario_done nowarn

[ "$FAILS" -eq 0 ] && echo "uvm_stress_selftest: all scenarios pass"
exit $((FAILS != 0))
