#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# uvm_stress.sh - lifetime stress for nvidia-uvm.ko (Track #2272): per-fd
# state, the pager objects of semaphore pools and managed ranges, VA space
# destroy, MM fd shutdown and module unload, all under concurrent load.
#
# For N minutes it keeps up to JOBS CUDA workers running -- cuda_smoke,
# cuda_vecadd and "cuda_vecadd managed", chosen at random, through the
# Linuxulator run script -- sends kill -9 to a random live worker every few
# seconds, runs open/close/mmap churn in parallel through
# tests/ioctl/uvm_ioctl_test, and every few seconds looks for new "Xid",
# "FATAL" and "panic" lines in dmesg and for a non-zero
# hw.nvidia_uvm.fatal_error.  With -U it also tries kldunload of nvidia-uvm
# while workers run (before the fix of #2272 the module could be freed under
# a pager destructor).  Two kinds of probe, alternating unless -H says one:
#   hold  a holder process keeps /dev/nvidia-uvm open: the unload must be
#         refused with "Device busy" (decided by the open file alone);
#   free  no holder: the unload may be refused (workers, or the maps of
#         killed workers still tearing down) or may succeed, and then the
#         module is loaded again from UVM_KO and the health check runs.
# Churn loops are paused around a free probe, and a loop reads
# hw.nvidia_uvm.gpus throughout (the unload must drain that handler).
# Any other kldunload failure (doas, a wrong name) is a FAIL, never a
# refusal.  After the load stops a last probe must unload the idle module.
#
# Usage: uvm_stress.sh [-n MINUTES] [-j JOBS] [-c CHURN] [-s SEED] [-U] [-H MODE] [-T] [-h]
#   -n  run time in minutes (default 10)
#   -j  concurrent CUDA workers (default 4)
#   -c  concurrent churn loops (default 2)
#   -s  seed of the random choices (default: the time)
#   -U  also try to unload the module under load (needs doas)
#   -H  with -U, the probe kind: hold, free or alternate (default alternate)
#   -T  self-test the kernel log filter against known lines and exit
# Environment:
#   GB10_WORK   work root (default $HOME/work/gb10); logs go to
#               $GB10_WORK/logs/uvm-stress-<time>/, scratch to $GB10_WORK/tmp
#   RUN_CUDA    the run script (default
#               $GB10_WORK/nvidia-gb10-freebsd-uvm0/scripts/run-linux-cuda.sh)
#   UVM_KO      the module file the -U probe reloads (default
#               /boot/modules.nvtest/nvidia-uvm.ko)
#   UVM_STRESS_NO_DEVICE_CHECK  set to 1 to skip the /dev/nvidia-uvm check
#               (self test with a fake RUN_CUDA)
# Run as the ordinary user, with nvidia.ko and nvidia-uvm.ko loaded.
# Exit status: 0 the run was clean; 1 a worker failed other than by kill -9
# (a worker whose log shows a failed open of the device, across a "free"
# unload that succeeded, is excused and logged), no worker finished OK, a
# churn loop failed, the kernel log has a new "nvidia-uvm: ... failed",
# "error" or "could not" line, hw.nvidia_uvm.gpus is missing after a reload,
# the holder could not be started, a worker was still alive 120 s after the load stopped, the dmesg
# or fatal check fired, an unload succeeded while the holder had the device
# open, an unload failed for any reason but "Device busy", a reload failed,
# or the final idle unload did not happen; 2 usage, a missing prerequisite or
# a holder that did not open the device.

set -u

PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
LC_ALL=C
export PATH LC_ALL

MINUTES=10
JOBS=4
CHURN=2
SEED=$(date +%s)
UNLOAD=0
HMODE=alternate
SELFTEST=0
# the kernel-log lines that mean trouble.  The module logs as "nvidia-uvm: ..."
# (UVM_NAME in sys/uvm.h); -T checks this expression against such lines.
TROUBLE_RE='Xid|FATAL|panic|Fatal|nvidia[-_]uvm: .*(failed|error|could not)'

# -T: the filter must catch the module's own failure lines and let its normal ones pass
selftest_filter() {
	bad=0
	while IFS= read -r line; do
		if printf '%s\n' "$line" | grep -qE "$TROUBLE_RE"; then
			echo "selftest: caught:  $line"
		else
			echo "selftest: MISSED:  $line"
			bad=1
		fi
	done <<'EOT'
nvidia-uvm: RM session creation failed: NV_ERR_GENERIC
nvidia-uvm: hw.nvidia_uvm.gpus could not be registered
nvidia-uvm: FATAL: channel timeout (NV_ERR_TIMEOUT); every later request fails
NVRM: Xid (PCI:0000:01:00): 31, pid=1
nvidia_uvm: an error line
EOT
	while IFS= read -r line; do
		if printf '%s\n' "$line" | grep -qE "$TROUBLE_RE"; then
			echo "selftest: FALSE ALARM: $line"
			bad=1
		else
			echo "selftest: passed:  $line"
		fi
	done <<'EOT'
nvidia-uvm: loaded: /dev/nvidia-uvm and /dev/nvidia-uvm-tools (spec r1, phase 1)
nvidia-uvm: unload refused: 1 open files, 0 pager objects, a GPU or a VA space is still in use
nvidia-uvm: unloaded
EOT
	return $bad
}

usage() {
	sed -n '/^# Usage:/,/^# Run as/p' "$0" | sed 's/^# \{0,1\}//'
}

while getopts n:j:c:s:UH:Th opt; do
	case $opt in
	n) MINUTES=$OPTARG ;;
	j) JOBS=$OPTARG ;;
	c) CHURN=$OPTARG ;;
	s) SEED=$OPTARG ;;
	U) UNLOAD=1 ;;
	H) HMODE=$OPTARG ;;
	T) SELFTEST=1 ;;
	h) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done
if [ "$SELFTEST" -eq 1 ]; then
	selftest_filter
	exit $?
fi
case $HMODE in
hold | free | alternate) ;;
*) echo "uvm_stress: -H is hold, free or alternate, not $HMODE" >&2; exit 2 ;;
esac
for v in "$MINUTES" "$JOBS" "$CHURN" "$SEED"; do
	case $v in
	'' | *[!0-9]*) echo "uvm_stress: not a number: $v" >&2; exit 2 ;;
	esac
done
[ "$JOBS" -ge 1 ] || { echo "uvm_stress: -j must be at least 1" >&2; exit 2; }

WORK=${GB10_WORK:-$HOME/work/gb10}
RUN_CUDA=${RUN_CUDA:-$WORK/nvidia-gb10-freebsd-uvm0/scripts/run-linux-cuda.sh}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/../.." && pwd)

if [ ! -x "$RUN_CUDA" ] && [ ! -f "$RUN_CUDA" ]; then
	echo "uvm_stress: no run script at $RUN_CUDA" >&2
	exit 2
fi
if [ "${UVM_STRESS_NO_DEVICE_CHECK:-0}" != 1 ] && [ ! -c /dev/nvidia-uvm ]; then
	echo "uvm_stress: /dev/nvidia-uvm is missing; load nvidia.ko and nvidia-uvm.ko first" >&2
	exit 2
fi

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOGDIR=$WORK/logs/uvm-stress-$STAMP
mkdir -p "$LOGDIR" "$WORK/tmp" || exit 2
T=$(mktemp -d "$WORK/tmp/uvm-stress.XXXXXXXX") || exit 2
mkdir -p "$T/w" "$T/c"

cleanup() {
	for p in $(cat "$T"/w/*.pid "$T"/c/*.pid 2>/dev/null); do
		kill -9 "$p" 2>/dev/null
	done
	[ -n "${HOLDER:-}" ] && kill "$HOLDER" 2>/dev/null
	:
	rm -rf "$T"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

# the churn harness, built natively
if ! cc -std=c11 -Wall -Wextra -I "$REPO/sys" -o "$T/uvm_ioctl_test" "$REPO/tests/ioctl/uvm_ioctl_test.c" 2>"$LOGDIR/churn-build.log"; then
	echo "uvm_stress: uvm_ioctl_test does not build (see $LOGDIR/churn-build.log)" >&2
	exit 2
fi

COUNTER=0
# rnd N: a number in [0, N); deterministic for a seed
rnd() {
	COUNTER=$((COUNTER + 1))
	awk -v s="$SEED" -v c="$COUNTER" -v n="$1" 'BEGIN { srand(s * 1000003 + c); print int(rand() * n) }'
}

SPAWNED=0
KILLED=0
OK=0
FAILED=0
CHURNS=0
UNLOAD_REFUSED=0
UNLOAD_DONE=0
HOLD_REFUSED=0
EXCUSED=0
PROBES=0
HOLDER=
RESULT=0
DEADLINE=$(( $(date +%s) + MINUTES * 60 ))
LASTCHECK=0
LASTKILL=0
LASTUNLOAD=0

log() {
	printf '%s %s\n' "$(date -u +%H:%M:%SZ)" "$*" | tee -a "$LOGDIR/run.log"
}

alive() {
	kill -0 "$1" 2>/dev/null
}

# the kernel log: dmesg (the message buffer is root-only on amp001), else doas dmesg, else the syslog copy
kernlog() {
	/sbin/dmesg 2>/dev/null || doas -n /sbin/dmesg 2>/dev/null || tail -n 20000 /var/log/messages 2>/dev/null
}

trouble() {
	kernlog | sed -e 's/^[A-Z][a-z][a-z] [ 0-9][0-9] [0-9:]* [^ ]* kernel: //' | grep -E "$TROUBLE_RE" | sort -u
}
trouble >"$T/dmesg.base"
FATAL0=$(sysctl -n hw.nvidia_uvm.fatal_error 2>/dev/null || echo 0)

check_health() {
	trouble >"$T/dmesg.now"
	new=$(comm -13 "$T/dmesg.base" "$T/dmesg.now")
	if [ -n "$new" ]; then
		log "FAIL: new trouble in dmesg:"
		printf '%s\n' "$new" | tee -a "$LOGDIR/run.log"
		RESULT=1
	fi
	fe=$(sysctl -n hw.nvidia_uvm.fatal_error 2>/dev/null || echo 0)
	if [ "$fe" != "$FATAL0" ]; then
		log "FAIL: hw.nvidia_uvm.fatal_error is $fe (was $FATAL0)"
		RESULT=1
	fi
}

# harvest the workers that exited: success, kill -9 or a failure
reap() {
	for pf in "$T"/w/*.pid; do
		[ -e "$pf" ] || continue
		id=${pf##*/}
		id=${id%.pid}
		p=$(cat "$pf")
		if alive "$p"; then
			continue
		fi
		wait "$p" 2>/dev/null
		rc=$?
		if [ -e "$T/w/$id.killed" ]; then
			:
		elif [ "$rc" -eq 0 ]; then
			OK=$((OK + 1))
		elif [ -e "$T/w/$id.gap" ] && grep -qE 'No such file or directory|Device not configured|No such device|ENXIO|ENOENT' "$LOGDIR/w$id.log" 2>/dev/null; then
			EXCUSED=$((EXCUSED + 1))
			log "worker $id ($(cat "$T/w/$id.name")) exited $rc across an unload that succeeded, on a failed open of the device: excused; see $LOGDIR/w$id.log"
		else
			FAILED=$((FAILED + 1))
			log "FAIL: worker $id ($(cat "$T/w/$id.name")) exited $rc; see $LOGDIR/w$id.log"
			RESULT=1
		fi
		rm -f "$pf" "$T/w/$id.name" "$T/w/$id.killed" "$T/w/$id.gap"
	done
}

live_workers() {
	ls "$T"/w/*.pid 2>/dev/null | wc -l | tr -d ' '
}

spawn_worker() {
	SPAWNED=$((SPAWNED + 1))
	id=$SPAWNED
	case $(rnd 3) in
	0) set -- cuda_smoke ;;
	1) set -- cuda_vecadd ;;
	*) set -- cuda_vecadd managed ;;
	esac
	printf '%s\n' "$*" >"$T/w/$id.name"
	sh "$RUN_CUDA" "$@" >"$LOGDIR/w$id.log" 2>&1 &
	echo $! >"$T/w/$id.pid"
}

# kill -9 a random live worker (the file name carries the id)
kill_one() {
	set -- $(ls "$T"/w/*.pid 2>/dev/null)
	[ $# -gt 0 ] || return 0
	shift "$(rnd $#)" 2>/dev/null || true
	[ $# -gt 0 ] || return 0
	pf=$1
	id=${pf##*/}
	id=${id%.pid}
	p=$(cat "$pf")
	if alive "$p"; then
		: >"$T/w/$id.killed"
		kill -9 "$p" 2>/dev/null && KILLED=$((KILLED + 1))
	fi
}

# churn loop i: open, close and mmap through the ioctl harness, until the deadline
churn_loop() {
	(
		while [ "$(date +%s)" -lt "$DEADLINE" ]; do
			# busy first, then the quiesce check, so a probe that sets the flag and
			# then sees no busy file knows no iteration is, or will be, in flight
			: >"$T/c/$1.busy"
			if [ -e "$T/quiesce" ]; then
				rm -f "$T/c/$1.busy"
				sleep 0.2
				continue
			fi
			"$T/uvm_ioctl_test" >>"$LOGDIR/churn$1.log" 2>&1 || echo "churn $1 failed" >>"$LOGDIR/churn$1.fail"
			echo x >>"$LOGDIR/churn$1.count"
			rm -f "$T/c/$1.busy"
		done
	) &
	echo $! >"$T/c/$1.pid"
}

# a loop that reads hw.nvidia_uvm.gpus all the time: the handler takes
# uvm.lock, and an unload must drain it before the lock is destroyed.  It
# fails when the file is missing during the unload window; that is expected
# and not counted.
sysctl_loop() {
	(
		while [ "$(date +%s)" -lt "$DEADLINE" ]; do
			sysctl -n hw.nvidia_uvm.gpus >/dev/null 2>&1
			sleep 0.05
		done
	) &
	echo $! >"$T/c/sysctl.pid"
}

# stop the churn loops between iterations (free rounds): the module is
# briefly absent, and an iteration that meets the gap would fail on its own
quiesce_churn() {
	: >"$T/quiesce"
	n=0
	while ls "$T"/c/*.busy >/dev/null 2>&1; do
		n=$((n + 1))
		if [ "$n" -gt 300 ]; then
			log "FAIL: the churn loops did not go idle in 60 s"
			RESULT=1
			return 1
		fi
		sleep 0.2
	done
}
resume_churn() {
	rm -f "$T/quiesce"
}

# try to unload the module under load.  The loaded file is nvidia-uvm.ko (a
# hyphen), so kldunload names the file; the reload uses UVM_KO, because the
# module may live outside module_path.
UVM_KO=${UVM_KO:-/boot/modules.nvtest/nvidia-uvm.ko}

holder_alive() {
	[ -n "${HOLDER:-}" ] && kill -0 "$HOLDER" 2>/dev/null
}
# start a holder that keeps /dev/nvidia-uvm open, and prove it does
holder_start() {
	holder_alive && return 0
	sh -c 'exec 3</dev/nvidia-uvm; exec sleep 86400' &
	HOLDER=$!
	sleep 1
	if ! holder_alive; then
		log "FAIL: the holder exited: /dev/nvidia-uvm could not be opened"
		RESULT=1
		HOLDER=
		return 1
	fi
	if ! fstat -p "$HOLDER" 2>/dev/null | grep -q 'nvidia-uvm'; then
		log "FAIL: the holder (pid $HOLDER) does not show /dev/nvidia-uvm in fstat"
		RESULT=1
		return 1
	fi
}
holder_stop() {
	holder_alive || { HOLDER=; return 0; }
	kill "$HOLDER" 2>/dev/null
	wait "$HOLDER" 2>/dev/null
	HOLDER=
}

# one attempt; returns 0 when the module is still loaded and 1 when it was
# unloaded (and reloaded).  A refusal is only "Device busy".
unload_probe() {
	held=0
	holder_alive && held=1
	if doas kldunload nvidia-uvm >"$T/unload.out" 2>&1; then
		UNLOAD_DONE=$((UNLOAD_DONE + 1))
		if [ "$held" -eq 1 ]; then
			log "FAIL: nvidia-uvm unloaded while a holder kept /dev/nvidia-uvm open (the unload gate did not hold)"
			RESULT=1
		fi
		for pf in "$T"/w/*.pid; do
			[ -e "$pf" ] && : >"${pf%.pid}.gap"
		done
		log "unloaded with nothing open ($(live_workers) workers recorded); reloading"
		doas kldload "$UVM_KO" >>"$LOGDIR/run.log" 2>&1 || {
			log "FAIL: kldload $UVM_KO after an unload failed"
			RESULT=1
			return 1
		}
		[ -c /dev/nvidia-uvm ] || { log "FAIL: /dev/nvidia-uvm is missing after the reload"; RESULT=1; }
		sysctl -N hw.nvidia_uvm.gpus >/dev/null 2>&1 || { log "FAIL: hw.nvidia_uvm.gpus does not exist after the reload"; RESULT=1; }
		check_health
		return 1
	fi
	if grep -q 'Device busy' "$T/unload.out"; then
		UNLOAD_REFUSED=$((UNLOAD_REFUSED + 1))
		[ "$held" -eq 1 ] && HOLD_REFUSED=$((HOLD_REFUSED + 1))
		return 0
	fi
	log "FAIL: kldunload nvidia-uvm failed for a reason other than Device busy: $(tr '\n' ' ' <"$T/unload.out")"
	RESULT=1
	return 0
}

# a probe of the kind this round calls for
probe_round() {
	PROBES=$((PROBES + 1))
	case $HMODE in
	hold) kind=hold ;;
	free) kind=free ;;
	*) if [ $((PROBES % 2)) -eq 1 ]; then kind=hold; else kind=free; fi ;;
	esac
	if [ "$kind" = hold ]; then
		holder_start || return 0
		unload_probe || true
		return 0
	fi
	holder_stop
	quiesce_churn || return 0
	unload_probe || true
	resume_churn
}

if [ "$UNLOAD" -eq 1 ] && [ "$HMODE" != hold ]; then
	sysctl_loop
fi
log "start: $MINUTES min, $JOBS workers, $CHURN churn loops, seed $SEED, unload probe $UNLOAD, logs $LOGDIR"
i=1
while [ "$i" -le "$CHURN" ]; do
	churn_loop "$i"
	i=$((i + 1))
done

while [ "$(date +%s)" -lt "$DEADLINE" ] && [ "$RESULT" -eq 0 ]; do
	now=$(date +%s)
	reap
	while [ "$(live_workers)" -lt "$JOBS" ]; do
		spawn_worker
	done
	if [ $((now - LASTKILL)) -ge $((3 + $(rnd 6))) ]; then
		kill_one
		LASTKILL=$now
	fi
	if [ "$UNLOAD" -eq 1 ] && [ $((now - LASTUNLOAD)) -ge 20 ]; then
		probe_round
		LASTUNLOAD=$now
	fi
	if [ $((now - LASTCHECK)) -ge 5 ]; then
		check_health
		LASTCHECK=$now
	fi
	sleep 1
done

# stop the load: let the workers finish (or be killed), then the last checks
log "stopping: waiting for workers"
for _ in $(seq 1 120); do
	reap
	[ "$(live_workers)" -eq 0 ] && break
	sleep 1
done
if [ "$(live_workers)" -gt 0 ]; then
	log "FAIL: $(live_workers) workers still alive 120 s after the load stopped (hung): $(cat "$T"/w/*.name 2>/dev/null | tr '\n' ';')"
	RESULT=1
	for p in $(cat "$T"/w/*.pid 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
	sleep 2
	reap
fi
for p in $(cat "$T"/c/*.pid 2>/dev/null); do wait "$p" 2>/dev/null; done
for f in "$LOGDIR"/churn*.fail; do
	[ -e "$f" ] && { log "FAIL: $(basename "$f") (see its log)"; RESULT=1; }
done
if [ "$OK" -eq 0 ] && [ "$SPAWNED" -gt "$KILLED" ]; then
	log "FAIL: no worker finished OK ($SPAWNED spawned, $KILLED killed, $EXCUSED excused): the run proved nothing"
	RESULT=1
fi
CHURNS=$(cat "$LOGDIR"/churn*.count 2>/dev/null | wc -l | tr -d ' ')
check_health
if [ "$UNLOAD" -eq 1 ]; then
	# nothing is open now (the holder first): the module must go, and must come back
	holder_stop
	before=$UNLOAD_DONE
	unload_probe || true
	if [ "$UNLOAD_DONE" -le "$before" ]; then
		log "FAIL: the idle module did not unload ($(tr '\n' ' ' <"$T/unload.out"))"
		RESULT=1
	fi
fi

log "summary: spawned $SPAWNED, killed $KILLED, ok $OK, failed $FAILED, churn runs $CHURNS, probes $PROBES, unload refused $UNLOAD_REFUSED (with a holder $HOLD_REFUSED), unloaded $UNLOAD_DONE, workers excused $EXCUSED"
if [ "$RESULT" -eq 0 ]; then
	log "PASS"
else
	log "FAIL (logs in $LOGDIR)"
fi
exit "$RESULT"
