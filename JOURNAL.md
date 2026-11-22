# JOURNAL (uvm-2272-uaf)

## 2026-11-20T07:26:40Z
- Picked up #2272; read brief, PROTOCOL. Audited uvm_main.c, uvm_mmap.c, uvm_va_space.c lifetime code and FreeBSD devfs/device_pager sources.
- Next: fix candidates C1 (sema pager ref race), C2 (unload vs pager dtor tail / TOCTOU), tests, stress script, panic doc.

## 2026-11-20T07:35:54Z
- Fixes written: C1 sema pager reference race (ctor owns the object's memory ref; mapper always drops its own), C2 unload under live module code (open_fds + pager_objs counters, unloading mark under uvm.lock, pm_lock barrier + pause, counter dropped last in every destructor; the managed pager dtor used to drop managed_objs before its sx_sunlock(pm_lock) on freed module data: matches x8=uvm+0, x6=deadc0de).
- Tests: tests/unit/unload_state_test.c, tests/unit/pager_ref_test.c (old rule red, new green); sh tests/run_unit_tests.sh all ok; module builds with base lld, plain and with INVARIANTS (sha a6016404, ~/work/gb10/logs/uvm-2272/nvidia-uvm.ko.invariants).
- Stress script tests/stress/uvm_stress.sh (self-tested with fake runner); docs/panic-capture.md.
- Next: commit through the gate, push branch, comment on #2272, hand off. Resume: if commit not done, run git status and commit with ~/work/gb10/tmp/COMMIT-MSG-2272-1.txt.
- 2026-11-20T07:43:22Z committed 9cb8134 and pushed; commented on #2272. Resume: nothing left; wait for test boot results.
- 2026-11-20 fixed 5 -Wcast-qual in uvm_channel.c (log pointers keep volatile: cast to volatile void *); amd64 cross-check, unit tests, base-lld build green.

## 2026-11-20T09:56Z
- Resumed after the 09:30 panic/reboot (tree clean at 63777bf, no gate was running). Working review findings 1a-e, 2, 3, 4, 5, 6 of reviews/uvm-2272-opus-2026-11-20.md. No module loads, no stress runs.
- Next: uvm_pager_ref.h + tests red/green, then sysctl ctx, uvm_unload comment, mmap comment, stress script, README.

## 2026-11-20T10:20Z
- All review findings fixed in the tree (pager_ref.h, sysctl ctx, unload comment/pause, mmap comment, stress script, README, tests with call-site pins). Gates green. Next: commit, push, Track comment. Resume: git status; commit.

## 2026-11-20T10:50Z
- Re-review fixes 1-7 done in the tree (quiesce, filter regex + -T self-test, sysctl loop + pins, bounded worker excuse, no-arg drops_ref, docs, holder FAIL not exit 2). Next: gate, commit, push, Track.

## 2026-11-20T12:15Z (combo worktree)
- Fixes for the hardware stress hang: holder marker (no fstat), stop flag + bounded wait for churn/sysctl loops, UUID lookup WARN vs baseline, fake-mode selftest (red first, logs/uvm-2272/stress_selftest_red.txt). Next: gate, commit, push, Track.

## 2026-11-21T04:13:44Z (#2373 sec)
- Red first: client_owner_test.c against the old-behaviour stub header (log ~/work/gb10/logs/uvm-2373/model_red.txt), then green. Check in uvm_ioctl.c for the five rmCtrlFd requests; pins test with 11 plants; probe tests/ioctl/uvm_owner_probe.c builds. Module built with base lld (Linker: LLD, undefined nvUvmInterfaceClientOwnedByFile). man page SECURITY section, README section.
- Needs the nvidia.ko of nvidia-gb10-freebsd agent/2373-nv-client-owner. Next: gate round 1 both repos, commit, push, Track, Honcho. Resume: git status.

## 2026-11-21T05:37:10Z
- Gate round 1 (receipt 45b1b792): pins strengthened (status returned, first call, moved-after and ignore-status plants), probe hardened (ioctl failure is FAIL, uid checks, closed fd row), cause logging, man/README. Rebuilt LLD. Next: round 2 then commit and push.

## 2026-11-21T07:00:51Z (#2373 fix round 3)
- uvm side in tree: CAP_IOCTL only, MODULE_DEPEND nvidia 2,2,2 (+pin), uvm glue harness (tests/client_owner_glue_test.sh, 23 mutants red), pins on comment-stripped text (+4 plants), PROVENANCE paragraph, C-Q Q-013 filed in uvm-qa, probe rows (/dev/nvidia0, dup, SCM_RIGHTS pass, SMC-only, P2P, setgroups, -j JSON), tests/ioctl/uvm_owner_race.c (4 loops), uvm_stress.sh -O, run_owner_boot.sh. Not yet committed. nv gate round 2 running; nv commit then phase0 merge branch; then uvm gate rounds.

## 2026-11-21T07:07:12Z
- uvm worktree: all suites pass in tree (98 ok, 0 FAIL), module built LLD. Waiting for the p0 nv gate, then uvm gate round 1.

## 2026-11-21T07:13:10Z
- uvm gate round 1 receipt 9e2a08f5 fixed (fget_cap + limited ioctl list refusal, race loops rewritten with assertions and watchdog, probe fixes, boot script, stress baseline, man). Round 2 running (gate-u2-2.log). Module rebuilt with base LLD.

## 2026-11-21T07:42:11Z (#2373 follow-up commits)
- Orchestrator: no round 3 on old commits. New uvm commit: empty ioctl list (fc_nioctls != -1, harness case, old-form mutant, probe row), race loop 4 fd leak, -j FAIL rows, README wording. Then nv commit (tombstone, stranger EPERM, 64-bit gen, rows+mutants), commit staged JOURNAL in wt-nv-2373, re-merge into phase0. Each 2 rounds.

- round 1 receipt 79929d93: restored probe control rows (my splice dropped them), enum, open_failed, README. Round 2 running.

## 2026-11-22 (#2373 B-1, new commit on 00e6902, own 2-round budget)
- B-1: the boot kit read RmClientDataValidation (no such key) and failed only on 0. Now reads RmValidateClientData (nvrm_registry.h:626): off when (v & 0x3) == 0x2, (v & 0x30) == 0x20 or (v & 0xC0) == 0x80; 0 is the default; a source that cannot be read (failed kenv, missing hw.nvidia.registry sysctl, empty capture, key with no value) is RE-RUN, never default. Fixtures for each case plus plants in tests/client_owner_pins_test.sh; red on 00e6902 (the new pins test run against the old script prints 41 failing validation lines), green now. Gate round 1 receipt 71a372f34a0b45d96d842e4eb379b1a920512b59eec657a21874db1dada8d1e2; round 2 answers in .round/tmp/answers.txt.
