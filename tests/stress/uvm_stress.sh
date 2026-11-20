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
# while workers run: the kernel must answer EBUSY (before the fix of #2272
# it could free the module under a pager destructor); if the module does
# unload, because nothing was open, it is loaded again.
#
# Usage: uvm_stress.sh [-n MINUTES] [-j JOBS] [-c CHURN] [-s SEED] [-U] [-h]
#   -n  run time in minutes (default 10)
#   -j  concurrent CUDA workers (default 4)
#   -c  concurrent churn loops (default 2)
#   -s  seed of the random choices (default: the time)
#   -U  also try to unload the module under load (needs doas)
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
# Exit status: 0 the run was clean; 1 a worker failed other than by kill -9,
# the dmesg or fatal check fired, or an unload under load succeeded while a
# worker was open; 2 usage or a missing prerequisite.

set -u

PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
LC_ALL=C
export PATH LC_ALL

MINUTES=10
JOBS=4
CHURN=2
SEED=$(date +%s)
UNLOAD=0

usage() {
	sed -n '/^# Usage:/,/^# Run as/p' "$0" | sed 's/^# \{0,1\}//'
}

while getopts n:j:c:s:Uh opt; do
	case $opt in
	n) MINUTES=$OPTARG ;;
	j) JOBS=$OPTARG ;;
	c) CHURN=$OPTARG ;;
	s) SEED=$OPTARG ;;
	U) UNLOAD=1 ;;
	h) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done
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

# the log lines that mean trouble; the same line is the same line on every look
trouble() {
	kernlog | sed -e 's/^[A-Z][a-z][a-z] [ 0-9][0-9] [0-9:]* [^ ]* kernel: //' | grep -E 'Xid|FATAL|panic|Fatal|nvidia_uvm: .*(failed|error)' | sort -u
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
		else
			FAILED=$((FAILED + 1))
			log "FAIL: worker $id ($(cat "$T/w/$id.name")) exited $rc; see $LOGDIR/w$id.log"
			RESULT=1
		fi
		rm -f "$pf" "$T/w/$id.name" "$T/w/$id.killed"
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
			"$T/uvm_ioctl_test" >>"$LOGDIR/churn$1.log" 2>&1 || echo "churn $1 failed" >>"$LOGDIR/churn$1.fail"
			echo x >>"$LOGDIR/churn$1.count"
		done
	) &
	echo $! >"$T/c/$1.pid"
}

# try to unload the module under load; EBUSY is the right answer.  With -U a
# holder process keeps /dev/nvidia-uvm open for the whole run, so a refusal
# is certain to be owed: an unload that succeeds while the holder lives is a
# failure of the unload gate (Track #2272), whatever the workers are doing.
# The loaded file is nvidia-uvm.ko (a hyphen), so kldunload names the file;
# the reload uses UVM_KO, because the module may live outside module_path.
UVM_KO=${UVM_KO:-/boot/modules.nvtest/nvidia-uvm.ko}
holder_start() {
	sh -c 'exec 3</dev/nvidia-uvm; exec sleep 86400' &
	HOLDER=$!
	sleep 1
}
holder_alive() {
	[ -n "${HOLDER:-}" ] && kill -0 "$HOLDER" 2>/dev/null
}
unload_probe() {
	if ! doas kldunload nvidia-uvm >"$T/unload.out" 2>&1; then
		UNLOAD_REFUSED=$((UNLOAD_REFUSED + 1))
		return 0
	fi
	UNLOAD_DONE=$((UNLOAD_DONE + 1))
	if holder_alive; then
		log "FAIL: nvidia-uvm unloaded while a holder kept /dev/nvidia-uvm open (the unload gate did not hold)"
		RESULT=1
	elif [ "$(live_workers)" -gt 0 ]; then
		log "unload succeeded with $(live_workers) workers recorded; reloading (workers may have been between runs)"
	fi
	doas kldload "$UVM_KO" >>"$LOGDIR/run.log" 2>&1 || {
		log "FAIL: kldload $UVM_KO after an unload failed"
		RESULT=1
	}
}

log "start: $MINUTES min, $JOBS workers, $CHURN churn loops, seed $SEED, unload probe $UNLOAD, logs $LOGDIR"
[ "$UNLOAD" -eq 1 ] && holder_start
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
		unload_probe
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
for p in $(cat "$T"/c/*.pid 2>/dev/null); do wait "$p" 2>/dev/null; done
for f in "$LOGDIR"/churn*.fail; do
	[ -e "$f" ] && { log "FAIL: $(basename "$f") (see its log)"; RESULT=1; }
done
CHURNS=$(cat "$LOGDIR"/churn*.count 2>/dev/null | wc -l | tr -d ' ')
check_health
if [ "$UNLOAD" -eq 1 ]; then
	# nothing is open now: the module may go, and must come back
	unload_probe
fi

log "summary: spawned $SPAWNED, killed $KILLED, ok $OK, failed $FAILED, churn runs $CHURNS, unload refused $UNLOAD_REFUSED, unloaded $UNLOAD_DONE"
if [ "$RESULT" -eq 0 ]; then
	log "PASS"
else
	log "FAIL (logs in $LOGDIR)"
fi
exit "$RESULT"
