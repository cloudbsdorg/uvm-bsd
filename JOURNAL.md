# JOURNAL (uvm-2272-uaf)

## 2026-11-20T07:26:40Z
- Picked up #2272; read brief, PROTOCOL. Audited uvm_main.c, uvm_mmap.c, uvm_va_space.c lifetime code and FreeBSD devfs/device_pager sources.
- Next: fix candidates C1 (sema pager ref race), C2 (unload vs pager dtor tail / TOCTOU), tests, stress script, panic doc.

## 2026-11-20T07:35:54Z
- Fixes written: C1 sema pager reference race (ctor owns the object's memory ref; mapper always drops its own), C2 unload under live module code (open_fds + pager_objs counters, unloading mark under uvm.lock, pm_lock barrier + pause, counter dropped last in every destructor; the managed pager dtor used to drop managed_objs before its sx_sunlock(pm_lock) on freed module data: matches x8=uvm+0, x6=deadc0de).
- Tests: tests/unit/unload_state_test.c, tests/unit/pager_ref_test.c (old rule red, new green); sh tests/run_unit_tests.sh all ok; module builds with base lld, plain and with INVARIANTS (sha a6016404, ~/work/gb10/logs/uvm-2272/nvidia-uvm.ko.invariants).
- Stress script tests/stress/uvm_stress.sh (self-tested with fake runner); docs/panic-capture.md.
- Next: commit through the gate, push branch, comment on #2272, hand off. Resume: if commit not done, run git status and commit with ~/work/gb10/tmp/COMMIT-MSG-2272-1.txt.
