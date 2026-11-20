# Provenance

Clean-room record for `nvidia-uvm-freebsd` (Track #2101). The implementer of
this tree never opened, listed, searched or fetched any of the forbidden
paths named in its brief: the GPL tree
`open-gpu-kernel-modules-580.178.04/kernel-open/`, the describer's dirty
notes `cleanroom/dirty-notes/`, the live spec `cleanroom/uvm-spec/`, any
Linux kernel or drm-kmod source, or `~/rx7600`. Every `grep`/`find` over the
NVIDIA checkout was run on `src/` only.

## Specification

- Frozen spec `/home/mlapointe/work/gb10/cleanroom/uvm-spec-r1/` (revision
  r1), verified against `uvm-spec-r1.sha256` by recomputing every file's
  sha256 (`/sbin/sha256 -q`) before any reading: 14 files, all matching.
  Files read in full: `SPEC.md`, `01-user-abi.md`, `02-rm-interface.md`,
  `03-va-space-and-ranges.md`, `04-channels-and-copy-engine.md`,
  `05-page-tables.md`, `06-faults.md`, `08-locking.md`,
  `09-freebsd-facilities.md`, `10-test-plan.md`, `11-phase2-features.md`,
  `12-hal-per-generation.md`, `13-implementation-phases.md`.
  `07-memory-residency-migration.md` (phase 2) was not read.
- Questions to the describer: `/home/mlapointe/work/gb10/cleanroom/uvm-qa/QUESTIONS.md`
  (see "Questions" below). Answers: `uvm-qa/ANSWERS.md` (none at the time of
  writing).

## Permitted sources consulted, per change

### User ABI and status headers (`sys/nvidia_uvm_ioctl.h`, `sys/nvidia_uvm_status.h`)

- Spec 01 sections 2-7 (numbers, layouts, status codes, errno table, CPU UUID).
- `open-gpu-kernel-modules-580.178.04/src/common/sdk/nvidia/inc/nvstatuscodes.h`
  (MIT): the full NV_STATUS table (about 150 codes); the spec names the ones
  it uses and says the implementation should carry the whole table.
- `src/nvidia/interface/nv_uvm_user_types.h` (MIT): confirmation of the
  mapping/caching/format/element-bits/compression enumeration values and the
  page-size constants (the spec gives the same values).
- `src/common/inc/nvCpuUuid.h` (MIT): the 16-byte UUID type; the CPU UUID
  bytes come from spec 01 section 2.5.
- FreeBSD `sys/kern/sys_generic.c` (`user_ioctl`) and
  `sys/compat/linux/linux_ioctl.c` (`linux_ioctl`, `linux_ioctl_fallback`,
  `linux_ioctl_register_handler`), `sys/compat/linux/linux_errno.inc`: why
  a native FreeBSD caller needs a direction bit in the request word
  (ENOTTY otherwise), how the Linuxulator dispatches raw request numbers to
  a registered handler by their low 16 bits, and that FreeBSD `ENOATTR`
  reaches a Linux process as `ENODATA`.

### RM interface header (`sys/nvidia_uvm_rm.h`)

- Spec 02 sections 2-5 (layouts, entry points, callback table).
- `src/nvidia/interface/nv_uvm_types.h` (MIT) and
  `src/nvidia/inc/kernel/rmapi/nv_gpu_ops.h` (MIT): field names and
  argument orders of the structures and gpu-ops the RM core exposes; the
  spec's byte offsets were checked against them (all agree).
- `src/nvidia/arch/nvalloc/unix/src/rm-gpu-ops.c` (MIT): the `rm_gpu_ops_*`
  wrapper signatures the FreeBSD glue calls.

### FreeBSD nvidia.ko glue (phase 0, repository `nvidia-gb10-freebsd`, branch `agent/uvm-phase0`)

- NVIDIA's FreeBSD driver package `NVIDIA-FreeBSD-x86_64-580.178.04/src/nvidia/`:
  `nvidia_subr.c` (ISR, open/close of a device, suspend/resume, modevent),
  `nvidia_pci.c` (interrupt setup is an ithread handler, module
  dependencies), `nvidia_dev.c`, `nvidia_ctl.c`, `nvidia_linux.c` (how the
  OS layer registers a Linuxulator ioctl handler), `nv.h`, `nv-freebsd.h`
  (softc, stack zone macros, file private), `Makefile`, `nvidia-sources.mk`,
  `nvidia_sysctl.c` (UUID read through RM).
- `src/nvidia/arch/nvalloc/unix/src/unix_intr.c` (MIT): what `rm_isr`,
  `rm_isr_bh` and `rm_gpu_copy_mmu_faults` do (the latter is a no-op for a
  GSP client GPU); `os.c` (MIT): `osQueueMMUFaultHandler` is the RM caller
  of `nv_schedule_uvm_isr`; `osapi.c` (MIT): `rm_set_external_kernel_client_count`;
  `dynamic-power.c` (MIT): dynamic power references, not used by the FreeBSD
  OS layer, so GC6 blocking is the open device reference.
- `nvidia-gb10-freebsd` repository: `README.md`, `scripts/prepare-tree.sh`,
  `scripts/build-shim.sh`, `scripts/build-rmcore.sh`, `scripts/lib.sh`,
  `patches/*`, `tests/stubs_test.sh`, `tests/kmod_symbols_test.sh`,
  `src/gb10-stubs/nv-gb10-stubs.c` (build flow, patch and manifest format,
  test style, SPDX header style).

### Hardware method and entry encodings (HAL, page tables, faults)

- Spec 04, 05, 06, 12.
- Public class headers in `src/common/sdk/nvidia/inc/class/` (MIT):
  `clc369.h` (fault buffer entry fields), `clca6f.h`, `clc96f.h` (host
  methods, GPFIFO entry, MEM_OP and semaphore fields), `clcab5.h`, `clc8b5.h`
  (copy engine methods and LAUNCH_DMA fields), `clc076.h` (clear-faulted
  software class); `ctrl2080mc.h` (architecture and implementation ids).
  Used to cross-check the spec's tables; the spec's values were taken where
  both agree (they did in every case checked).

### FreeBSD kernel facilities

- `/home/mlapointe/src/csrc-90dcf68/sys`: `sys/conf.h`, `kern/kern_conf.c`
  (cdevsw, `d_mmap_single`, cdevpriv), `fs/devfs/devfs_int.h` (another
  file's cdev private data), `sys/file.h`, `vm/vm_pager.h`,
  `vm/device_pager.c`, `vm/vm_mmap.c` (`vm_mmap_cdev`),
  `compat/linuxkpi/common/src/linux_compat.c` (the in-tree pattern for a
  cdev pager fault returning a fake page), `vm/vm_extern.h` (vmspace
  references), `sys/taskqueue.h`, `sys/sx.h`, `sys/eventhandler.h`,
  `vm/vm_page.h`, `sys/malloc.h`, `conf/kmod.mk`, `modules/cuse/Makefile`,
  `modules/linux64/Makefile`, `modules/linux_common/Makefile`,
  `arm64/linux/linux_sysvec.c` (module names `linux64elf`, `linux_common`),
  `arm64/linux/linux_proto.h` (`struct linux_ioctl_args`),
  `tools/makeobjops.awk` and `tools/vnode_if.awk` (the interface headers
  the amd64 cross-compile check generates).
- For the vendor-neutral core (`sys/core/`): `sys/tree.h` (the red-black
  tree behind the range tree), `sys/queue.h`, `sys/sx.h`, `sys/malloc.h`,
  `sys/kernel.h` and `sys/systm.h` (`KASSERT`) behind `uvm_core_os.h`; the
  userland shim of that header uses pthread(3) and assert(3) from the base
  system only.

## Architecture: our own design decision

Owner decision (Mark, 2026-11-19): the driver is split into a vendor-neutral
core (`sys/core/`: VA spaces and ranges, residency and migration policy, the
fault-service queue on the FreeBSD VM system, device-memory accounting) and a
vendor backend layer (`sys/nvidia/`: the `/dev/nvidia-uvm` ioctl ABI libcuda
expects, the RM interface, the Blackwell HAL), joined by an ops table of our
own (`docs/ARCHITECTURE.md`).  This structure is not derived from anyone's
code; it is recorded here as a design decision and its consistency with the
spec goes through the describer's Q&A loop (Q-006).  The split is carried
out as opencode rounds after the flat phase-1 code landed.

## Decisions taken without a spec answer

Phase-1 simplifications, each stated in the file that makes it:

- The GPU's internal page tree (flat identity mappings) is not created: on
  Blackwell the copy engine addresses memory physically, pushbuffer and
  semaphores use RM's own mappings, page tables are written physically
  (`sys/uvm_gpu.c`).
- Page tables live in system memory (the spec's fallback location), one
  4K DMA page per table, zeroed and written by the copy engine, never by
  the CPU (`sys/uvm_page_tree.c`).
- One channel pool on the first usable copy engine, two channels, for
  every channel type; the pushbuffer is divided into 128 slots of the
  maximum push size instead of chunk rings (`sys/uvm_channel.c`).
- `UVM_MM_INITIALIZE` answers `NV_WARN_NOTHING_TO_DO` (allowed by 01
  section 3.4): no managed memory exists yet whose pages the GPU could
  touch during process exit (`sys/uvm_ioctl.c`; Q-005).
- A partial `UVM_UNMAP_EXTERNAL` removes the whole mapping and logs it,
  instead of splitting (`sys/uvm_va_space.c`).
- The TLB invalidation size takes the alignment-aware reading (Q-002).
- `EBADF` where the spec says `EBADFD` (no FreeBSD equivalent); mmap cannot
  check that the file offset equals the mapped address, since FreeBSD's
  device mmap path does not pass the address to the driver (`sys/uvm_mmap.c`).
- Suspend is vetoed (`NV_ERR_NOT_SUPPORTED`) while any GPU is registered,
  as 02 section 5 allows until phase 3 (`sys/uvm_gpu.c`).

- `NV_ERR_OBJECT_NOT_FOUND` becomes `ENOATTR` on FreeBSD (no `ENODATA`
  exists); the Linuxulator translates it to Linux `ENODATA`, which is what
  the spec's table requires the user to see.
- Native FreeBSD request encoding (`UVM_IOCTL_NATIVE`): FreeBSD's ioctl(2)
  refuses a request word without a direction bit, so a native caller wraps
  the raw number in `IOC_VOID` (see `sys/nvidia_uvm_ioctl.h`). The Linux
  path delivers the raw numbers unchanged through a registered handler.
- GC6 blocking in the glue is the open device reference (the FreeBSD OS
  layer has no runtime power management), see `nv-uvm-interface.c`.

### Lifetime audit and unload gate (Track #2272)

No NVIDIA, Linux or dirty-notes path was opened for this change.  Sources
read: this repository; the frozen spec r1 (`03-va-space-and-ranges.md`,
`08-locking.md`, `09-freebsd-facilities.md`); and the FreeBSD sources
`sys/fs/devfs/devfs_vnops.c` (cdevpriv, `devfs_fp_check`, `devfs_mmap_f`),
`sys/kern/kern_conf.c` (`destroy_devl`), `sys/vm/device_pager.c`
(`cdev_pager_allocate`, `dev_pager_dealloc`), `sys/vm/vm_pager.c`
(`vm_pager_object_lookup`), `sys/compat/linux/linux_ioctl.c` (handler
registry), `sys/kern/kern_shutdown.c` (panic path) and the `ddb(4)`,
`textdump(4)` and `ddb(8)` manual pages.

## Questions

Numbered questions filed in `uvm-qa/QUESTIONS.md`; each is resolved here
when its checked answer arrives in a new frozen revision.

| Question | Status | Decision meanwhile |
|---|---|---|
| Q-001 root entries RM fills at the page directory hand-over | open | read the root back, keep RM's indexes, refuse addresses that need them |
| Q-002 TLB invalidation size and the aligned-down base | open | conservative block that covers the range after alignment |
| Q-003 SET_OBJECT on subchannel 4 for copy-engine methods | open | as the spec states: subchannels 0 and 5 only |
| Q-004 the numEntries argument of the page directory hand-over | open | the root table's entry count (2 for MMU v3, 4 for v2) |
| Q-005 NV_WARN_NOTHING_TO_DO from UVM_MM_INITIALIZE in this phase | open | answered; release ordering comes with managed ranges |
| Q-006 the core/backend split against the spec's locking and teardown order | open | the mapping of spec objects to layers in docs/ARCHITECTURE.md |
| Q-007 the core device registry (probe by identity, two reference counts) against the spec's register-GPU order | open | the core reference brackets the spec's retain and release steps; the RM client handles stay backend-side |
