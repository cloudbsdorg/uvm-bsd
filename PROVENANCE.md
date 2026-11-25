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

## Track #2373: RM client ownership (rmCtrlFd and hClient)

The registration requests take an `rmCtrlFd` and an RM client handle that RM resolves with kernel
privilege. Spec r1 (01-user-abi.md, the `rmCtrlFd` paragraph) says the field is accepted and stored
but not validated by this driver generation and that RM validates the handles; for the
kernel-privilege entry points this driver uses (dup address space, dup memory, retain channel, GPU
info with client) that is not true, and this change deviates from the spec on purpose: before any RM
call the handle must belong to the nvidiactl file that `rmCtrlFd` names in the caller. The question
is Q-013 in `uvm-qa/QUESTIONS.md`; the spec erratum is the describer's to make. Sources consulted for
this change: this repository; spec r1 01, 02 and 08; the MIT headers `nvos.h` and `nv_escape.h` of
open-gpu-kernel-modules 580.178.04 (layouts and ioctl numbers only); the FreeBSD OS layer of the
NVIDIA 580.178.04 package (`nvidia_subr.c`, `nvidia_ctl.c`) and its prepared tree in
nvidia-gb10-freebsd; FreeBSD sources `sys/kern/kern_descrip.c`, `sys/fs/devfs/devfs_vnops.c`,
`sys/sys/file.h` and Capsicum's `sys/sys/caprights.h` (`CAP_IOCTL`). Nothing from the Linux kernel,
`kernel-open/` or the describer's notes was read. The rule is `sys/uvm_client_owner.h`; the
nvidia.ko side is `src/gb10-client-owner/` in nvidia-gb10-freebsd.

## 2026-11-25: PR 6 fixes on top of dch's zero-video-memory commits (Track #2504)

The review of dch's three commits (gate1, 2026-11-25) found two things wrong and one thing
untested.  What changed here:

- `sys/uvm_fb_state.h`: the frame buffer information now has three states instead of one
  boolean. `UVM_FB_ABSENT` means the GPU really has no video memory (RM cleared the whole
  description), `UVM_FB_BROKEN` means the information is broken or missing (the request
  failed, RM calls the frame buffer broken with `bZeroFb`, or the description is empty
  although the RM handed out a PMA object), and `UVM_FB_USABLE` means the GPU has video
  memory and RM describes it. `bZeroFb` is RM's "the frame buffer is broken" answer and no
  longer means "no video memory": only `UVM_FB_ABSENT` makes a missing PMA object normal,
  and a `UVM_FB_BROKEN` GPU fails its registration with a message.
- `sys/uvm_gpu.c`: the registration takes the PMA path from the verdict of the helper
  (`uvm_fb_pma_judge`), not from the raw status of the RM call, and the state of the frame
  buffer information is kept in `gpu->fb_state`.
- `sys/uvm_channel.c`: the tunable `hw.nvidia_uvm.sysmem_gpfifo` is read once, into a
  local, and the placement and the message are taken from the same value
  (`uvm_fb_gpfifo_place`), so that they cannot disagree. The RM buffer location of the
  GPFIFO ring and of the PUT word comes from `uvm_fb_gpfifo_location`.
- `tests/unit/fb_state_test.c`: every row of the three states, of the PMA verdict and of
  the GPFIFO placement, plus the registration and channel-allocation transitions driven
  through fakes.
- `tests/run_unit_tests.sh`: the two old call-site greps are replaced by pins that are
  shown to be able to fail: a mutant that calls broken information absent, a mutant that
  puts the GPFIFO in video memory without video memory, a mutant that ignores the PMA
  verdict, a mutant that ignores the GPFIFO verdict and a mutant that reads the tunable
  twice are each built, and the test or the pin must catch them.
- `man/nvidia-uvm.4`, `README.md`: what happens on a GPU with no video memory, and what
  happens to a GPU whose frame buffer information is broken.

Sources consulted for this change: this repository, and the facts about the RM recorded by
dch in the section above (`queryFbInfo` clears the description of a zero-FB GPU and makes no
query there, `bZeroFb` is the "FB is broken" answer otherwise, and the PMA is initialised
only where frame buffer regions exist). No RM source, Linux source or GPL source was read
for this change; the RM behaviour above is taken from the recorded provenance of the
change it corrects. The state machine itself, the verdict, the placement and the tests are
this change's own work.


## 2026-11-24: GPUs without video memory (GB10, branch `gb10-no-pma`)

On the GB10 SoC GPU (chip GB20B, PCI 10de:2e12) registration stopped at the request for the PMA
object: RM has no PMA where there is no frame buffer. What changed:

- `sys/uvm_fb_state.h` (new): the rule, as pure functions. A GPU has no video memory when the frame
  buffer description says so (`bZeroFb`) or shows no heap, no reserved heap and no allocatable
  address; `NV_ERR_OBJECT_NOT_FOUND` from `nvUvmInterfaceGetPmaObject` is normal on such a GPU and
  only there; the GPFIFO goes to system memory on such a GPU whatever the tunable says.
- `sys/uvm_gpu.c`: registration goes on without a PMA object on such a GPU, registers no eviction
  callbacks, and its failure path unregisters them only when they were registered. A GPU with video
  memory takes the same path as before.
- `sys/uvm_channel.c`, `sys/uvm.h`: the channel manager records where the GPFIFO goes and never asks
  RM for video memory on a GPU that has none (`hw.nvidia_uvm.sysmem_gpfifo=0` is ignored there).
- `tests/unit/fb_state_test.c` (new) checks every row of the rule; `tests/run_unit_tests.sh` pins the
  two call sites.

Permitted sources consulted, all under
`/home/dch/src/nvidia-gb10-freebsd/work/sources/open-gpu-kernel-modules-580.178.04/src/nvidia/`
(MIT) unless a full path is given:

- `src/kernel/rmapi/nv_gpu_ops.c`: `queryFbInfo` (a zero-FB GPU gets the description cleared and no
  query is made; `bZeroFb` is the "FB is broken" answer otherwise), `nvGpuOpsGetFbInfo`,
  `nvGpuOpsGetPmaObject` (the three checks that answer `NV_ERR_OBJECT_NOT_FOUND`),
  `deviceNeedsDummyAlloc`, `channelNeedsDummyAlloc`, `channelAllocate` (buffer location defaults and
  the allocations it makes), `nvGpuOpsAllocPhysical`, `nvGpuOpsGpuMalloc`, `nvGpuOpsMemoryAllocSys`,
  `nvGpuOpsMemoryAllocFb`, `queryCopyEngines`, `setCeCaps`, `nvGpuOpsQueryCesCaps`,
  `nvGpuOpsQueryCaps`, `getSysmemLinkInfo`, `nvGpuOpsSetPageDirectory`, and the lines that fill
  `sysmem` in the memory and channel descriptions.
- `src/kernel/gpu/mem_mgr/heap.c` (the PMA is marked initialised only when frame buffer regions
  exist), `src/kernel/gpu/mem_mgr/mem_mgr.c`, `src/kernel/gpu/mem_mgr/dma.c`
  (`deviceCtrlCmdDmaSetPageDirectory_IMPL`, the zero-FB order), `src/kernel/gpu/mem_mgr/vaspace_api.c`,
  `src/kernel/mem_mgr/video_mem.c`, `src/kernel/mem_mgr/virtual_mem.c`,
  `src/kernel/gpu/fifo/kernel_channel.c`, `src/kernel/gpu/fifo/arch/maxwell/kernel_channel_gm107.c`,
  `src/kernel/gpu/bus/kern_bus_ctrl.c`, `src/kernel/gpu_mgr/gpu_mgr.c`: short excerpts around the
  uses of `PDB_PROP_GPU_ZERO_FB` and `memmgrIsPmaInitialized`, found by `grep` run on `src/` only.
- This repository: `sys/uvm_gpu.c`, `sys/uvm_channel.c`, `sys/uvm_page_tree.c`, `sys/uvm_va_space.c`,
  `sys/uvm_faults.c`, `sys/uvm_backend_nvidia.c`, `sys/uvm_main.c`, `sys/uvm.h`,
  `sys/nvidia_uvm_rm.h`, `sys/nvidia_uvm_status.h`, `sys/uvm_unload_state.h`, the tests.

No FreeBSD source and no file of `nvidia-gb10-freebsd/src/gb10-uvm` was needed. None of the
forbidden paths was opened, listed, searched or fetched: no `kernel-open` directory, no Linux kernel
or drm-kmod source, nothing under `/home/dch/src/nvidia-uvm-freebsd`, `/home/dch/src/ogkm-ref` or
`/home/dch/ai/spark/ref`. Every search of the NVIDIA checkout ran on its `src/` subdirectory. The
observed behaviour (the kernel log lines of the failed registration) came with the task.
