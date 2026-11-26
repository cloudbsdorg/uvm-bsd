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

## 2026-11-22T02:10:00Z (#2373 reader fixes, new commit on 7c6e669, own 2-round budget)
- Orchestrator: fix leading-zero-89 (numbers read base 0 as strtoul: 0x hex, leading 0 octal, invalid is RE-RUN), grep-error-collapse (grep exit status: 1 no match, 2 or more RE-RUN), a FAIL stands when sysctl or kenv failed, remedy names the right tunable, colon/equals-glued key. Gate round 1 running.

## 2026-11-22T02:40:00Z (#2373 one-shot BE kit, new commit on e5d29d7, own 2-round budget)
- Reader fixes committed e5d29d7 (round 1 6e744db5, round 2 7645ca8e). Next commit: tests/ioctl/testboot (make-be.sh, rc one-shot, summarize.sh, vendored testboot-return pinned in VENDOR) and tests/testboot_kit_test.sh; run_owner_boot.sh takes PROBE_UID_A/B. Gate round 1 running; then the dry run on the real host (a BE built, not armed).

## 2026-11-22T03:10:00Z (#2373 kit safety fixes, new commit on 7fbca39, own 2-round budget)
- Orchestrator: a dead-man timer not pending loads nothing (KIT, return straight away); FAIL wins over KIT in summarize.sh; rc script, summarize.sh and testboot-return installed from the git archive; usr/local/etc/rc.conf.d scanned. Gate round 1 running; then destroy the old dry-run BE, build a fresh one, mount check, prove the dead-man timer pending.
- Round 1 receipt 2d6d5617b04b5dba52601f60cfc35b2629f316ed28175fcf86e8cf7b096f2530 (5 voices FINDINGS, answers .round/tmp/answers.txt). Folded in: the rc script falls back to a 0700 /var/db/nvco-testboot (never /tmp) and, when no run directory can be made at all, writes no results and still runs testboot-return; a dead-man timer disabled or not pending loads nothing.

## 2026-11-25 (#2504 PR 6 fixes on top of dch's gb10-no-pma commits, branch agent/dch6-fixes on 414c93d)
- Read the gate1 review (quality and falsification): bZeroFb ("the frame buffer is broken") was treated as
  proof that a GPU has no video memory, the tunable was sampled twice, and the runner only grepped for a
  substring that a mutant could keep.
- sys/uvm_fb_state.h: one boolean became three states (UVM_FB_USABLE, UVM_FB_ABSENT, UVM_FB_BROKEN).
  ABSENT is RM clearing the whole description; BROKEN is an error status, bZeroFb, or an empty
  description next to a PMA object the RM handed out (a PMA exists only where there is video memory).
  Only ABSENT makes a missing PMA normal; BROKEN fails the registration with a message.
- sys/uvm_gpu.c: the PMA path comes from uvm_fb_pma_judge(gpu->fb_state, status); the state is kept in
  gpu->fb_state (sys/uvm.h) and printed in the registration log line.
- sys/uvm_channel.c: hw.nvidia_uvm.sysmem_gpfifo is read once into sysmem_requested; the placement and
  the message come from uvm_fb_gpfifo_place(), so they cannot disagree; the RM buffer location comes from
  uvm_fb_gpfifo_location().
- tests/unit/fb_state_test.c: every row of the three states, of the PMA verdict and of the GPFIFO
  placement, plus the registration and the channel allocation driven through fakes. Red first: the mutant
  that calls broken information absent and the mutant that puts the GPFIFO in video memory without video
  memory both make the test fail (logs/dch6/).
- tests/run_unit_tests.sh: the two old greps are replaced by pins that are shown to be able to fail (the
  PMA verdict ignored, the GPFIFO verdict ignored, the tunable read twice).
- man/nvidia-uvm.4 and README.md: what happens on a GPU with no video memory, and that a GPU with broken
  frame buffer information is not registered at all.
- Module builds with -Werror against /usr/src/sys (aarch64, FreeBSD 16.0-CURRENT). No kldload, no reboot.

## 2026-11-25 (#2504 PR 6 fixes, pass B: the lizard gate)
- The gate refused tests/unit/fb_state_test.c:99 main: 137 lines of code over the limit of 100. The cases
  are tables now: one row per check, the same messages, and the row carries the line it is written on, so
  a failure still names the case that failed. Proven, not asserted: instrumented counters say 56 checks
  before and 56 after, the passing output is byte-identical (cmp), and both header mutants (broken read as
  absent, GPFIFO put in video memory without video memory) print the same messages.
- The same command also refused sys/uvm_channel.c: uvm_channel_manager_create had grown to 103 lines
  (the switch of pass A). The switch moved into uvm_channel_gpfifo_log(); the function is 88 lines now.
  The pinned strings are all still there: the tunable read once, the place from the helper, the is_sysmem
  call, and the UVM_FB_GPFIFO_SYSMEM_NO_VIDEO case. The pin mutants still fail the pins.
- The last line was sys/vnode_if.h: a copy of FreeBSD's generated header that the kernel build leaves in
  sys/, not repository source. The root .gitignore already said so; lizard honours a .gitignore only
  inside the directory it scans, so sys/.gitignore repeats the patterns.
- sh tests/run_unit_tests.sh passes (exit 0). Module builds with -Werror against /usr/src/sys (aarch64,
  FreeBSD 16.0-CURRENT). No kldload, no reboot.

## 2026-11-25 (Track #2444 pass B: the native driver and the optional Linux ioctl bridge)
- Pass A's work kept and finished: the driver is native (MODULE_DEPEND {kernel, nvidia}, no Linux
  symbol), the Linuxulator bridge is the separate optional module linux/nvidia_uvm_linux.ko, and the
  bridge interface is sys/uvm_bridge.[ch] (the driver exports uvm_bridge_fd_is_ours).
- Pass A left the tree red: sys/uvm_channel.c still used the old frame buffer names, and the new
  request numbering broke tests/unit/abi_layout_test.c and the ioctl harnesses. Fixed.
- Found a real bug in pass A's native encoding: uvm_ioctl_native_decode() compared the top 16 bits of
  the request word against IOC_VOID|group, but the group byte is below bit 16, so every native request
  decoded to 0 and every ioctl answered ENOTTY. The decode now checks the direction, the size, the
  group and the number; tests/unit/linux_bridge_test.c caught it.
- The request numbers are the driver's own now (UVM_REQ_*, dense and stable), and the Linux numbers
  live in the bridge header with a translation in both directions.
- sys/uvm_requests.h (new): the rule that the table covers every id exactly once; the module asks it
  at load and refuses to load when it does not.
- tests/kmod_deps_test.sh (new): the static load check (exact dependency sets, no Linux symbol in the
  driver, every undefined symbol resolves, the export exists), each check shown to be able to fail.
- "Unified memory" replaces "no video memory" everywhere (Mark, 2026-11-25): UVM_FB_UNIFIED,
  UVM_FB_DEDICATED, UVM_FB_PMA_UNIFIED, uvm_fb_is_unified(); the kernel message reads "unified memory
  (no dedicated memory); GPU memory is shared system memory". RM's own names (bZeroFb,
  PDB_PROP_GPU_ZERO_FB) are kept with a comment that RM calls this "zero framebuffer".
- make install installs both modules and both manual pages; the bridge is never loaded by the installer.
- sh tests/run_unit_tests.sh, sh tests/make_install_test.sh, the amd64 cross check and mandoc -T lint
  on both pages pass. Both modules build with -Werror against /usr/src/sys (aarch64, FreeBSD
  16.0-CURRENT) and are linked by the base lld. No kldload, no reboot.

## 2026-11-26 (Track #2444 pass C: the one request table)
- The gate refused pass B: lizard on linux/nvidia_uvm_linux_ioctl.h:112 (uvm_linux_request_id, 54
  branches, 114 lines) and its pair, and on tests/unit/linux_bridge_test.c:162 (main, 66 branches,
  121 lines); cpd on the two translation functions (115 lines each), on the enum of
  sys/nvidia_uvm_ioctl.h against the id list of the test (54 lines), and on the test's own list
  against itself (31 lines).
- The mapping is now ONE table: linux/uvm_linux_request_table.h, one row per request (the name,
  the driver's UVM_REQ_* id, the Linux number and the size of the parameter structure). The two
  translations are small lookups over it (9 lines, 3 branches each), and
  tests/unit/linux_bridge_test.c walks the same table instead of holding a copy of the lists.
- The table is GENERATED at build time: tools/gen-ioctl-table.sh (taken unchanged from
  skunkwerks/nvidia-uvm-freebsd, commit fbf8ed3, BSD-2-Clause) reads NVIDIA's MIT uvm_ioctl.h and
  prints one entry per request; tools/gen-uvm-request-table.sh joins those names with this
  repository's ids, Linux numbers and parameter structures, and checks that the Linux numbers are
  the ones NVIDIA's header carries. The generated file is committed, so a build needs no NVIDIA
  source; `make regen-ioctl-table NVIDIA_UVM_IOCTL_H=...` rewrites it and `make build` checks it.
- No behaviour change: the 53 pairs of the old switch and the 53 rows of the table are the same,
  compared name by name.
- The Linux request numbers of uvm-spec r1 01-user-abi.md section 2.1 are the ones NVIDIA's header
  carries (every one of them compares equal). The two initialisation requests are not in NVIDIA's
  Linux interface at all, so their numbers come from the specification alone and the unit test pins
  them.
- lizard -C 20 -L 100 -a 8 -w sys linux tests/unit prints nothing; scan-cpd over the changed files
  finds nothing (the only clone left in that scope, the module's GPU-id block against the core's
  id block, is in the base commit as well and is not this change's). Both modules build with
  -Werror against /usr/src/sys and are linked by the base lld. No kldload, no reboot.
