# nvidia-uvm for FreeBSD

A clean-room, BSD-2-Clause kernel module providing `/dev/nvidia-uvm` and
`/dev/nvidia-uvm-tools`, the unified virtual memory interface NVIDIA's
CUDA user-mode driver (libcuda, driver generation 580.178.04) needs beside
`nvidia.ko`. Targets FreeBSD 16.0-CURRENT on arm64 and amd64.

It is written from a functional specification only (`uvm-spec`, revision
r1, see `.clean-room`), never from NVIDIA's Linux driver; `PROVENANCE.md`
records every source consulted. Track #2101.

## Status (2026-11-20, commit 256297e)

- **Running:** this version is loaded and running on an Ampere Altra (FreeBSD 16.0-CURRENT
  arm64) with a GeForce RTX 5060 Ti, beside `nvidia.ko` 580.178.04 from
  [nvidia-gb10-freebsd](https://github.com/cloudbsdorg/nvidia-gb10-freebsd).
- **Linux CUDA works through the Linuxulator:**
  - `cuInit` returns in about 1 s;
  - context creation, memory copies and managed memory work;
  - a kernel launch (vectorAdd over 1,048,576 floats) gives 0 mismatches;
  - llama.cpp b12531 runs on the CUDA backend: with Llama-3.2-3B-Instruct Q4_K_M, prompt
    processing over 512 tokens reaches 8728 tokens per second, and generation over 128 tokens
    166.2.
- **Stress test:** 30 minutes, 5992 CUDA processes, 220 of them killed with `kill -9`, no
  failure. All 90 attempts to unload the module while the device was open were refused. No Xid
  error, no lock-order reversal, no panic.
- **Fixes in this version:**
  - unloading while the device is open is refused (it used to free memory still in use,
    Track #2272);
  - page tables come from one-page DMA allocations, and Giant is dropped around ioctls (CUDA
    used to take 12-15 minutes to start, Track #2295);
  - copy-engine memsets use a pitch source layout (block-linear caused an Xid 71, 5350a13).
- **Packaging:** the CloudBSD ports overlay has a port for this module,
  `graphics/nvidia-uvm-kmod`. Its record is amd64-only for now, because aarch64 packages also
  need the 580 `nvidia-kmod` built on an aarch64 builder. The module itself is tested only on
  arm64, by hand (above). No package is published yet. This repository is private, so an
  operator stages the source on the builder.
- **Not done yet:** peer access, device P2P, access counters and tools trackers (later phases);
  a test on amd64 hardware; a test on the DGX Spark (GB10).

## What this phase does

Phases 0 and 1 of the specification's plan:

| Area | State |
|---|---|
| `nvidia.ko` glue (phase 0) | in the `nvidia-gb10-freebsd` repository, branch `agent/uvm-phase0`: `nvidia.ko` exports the 60 `nvUvmInterface*` entry points, runs UVM's interrupt top half before RM's own service, and hands UVM the fault-buffer stacks |
| Device nodes, ioctl dispatch, fd state machine | done: raw Linux request numbers through the Linuxulator, a native encoding for FreeBSD callers, every structure copied in and out whole, `rmStatus` carries the result |
| `UVM_INITIALIZE`, `UVM_MM_INITIALIZE`, teardown | done; the secondary fd holds a users reference on the bound map and its release runs the mm shutdown (01 section 3.4, 03 section 2.1); answers `NV_OK` |
| GPU registration through RM | done: device, address space, fault buffers, interrupt ownership, PMA object, copy-engine channels; Blackwell GB20x has the HAL, other generations are refused at registration |
| GPU VA space registration (page directory hand-over) | done, page tables in system memory written by the copy engine |
| Channel registration with resource mapping | done |
| External ranges, `cudaMalloc` mappings, sparse mappings, unmap, free, validate | done |
| Semaphore pools with mmap | done |
| Dynamic-parallelism regions | done |
| Replayable faults | fetched, attributed and cancelled (no managed memory yet); non-replayable faults reported to RM |
| Managed memory | step 1: an mmap of a free address creates a managed range, CPU resident in zeroed system memory and mapped on every GPU (no fault servicing, no migration); it ends with the last unmap or the VA space, whichever is first |
| Policies, migration, range groups | bookkeeping on managed ranges; on other addresses `NV_ERR_INVALID_ADDRESS` as the spec says |
| Peer access, device P2P, access counters, tools trackers | later phases: `NV_ERR_NOT_SUPPORTED`, logged once |

Locking, memory and logging (Track #2295):

- The Linuxulator calls a registered ioctl handler with Giant held. `uvm_linux_ioctl` drops Giant around `kern_ioctl` (`DROP_GIANT`/`PICKUP_GIANT`); nothing in the module relies on Giant (our `sx` and `mtx` locks, task queues not bound to Giant).
- Page tables and probe pages come from a one-page DMA tag (`UVM_GPU_DMA_MAXSIZE`), because `bus_dmamem_alloc` allocates the tag's whole `maxsize`. `tests/dma_tag_size_test.sh` fails if a tag is larger than its allocations.
- The long page-table loops call `maybe_yield()`; the map loop also stops with `NV_ERR_SIGNAL_PENDING` on a pending signal and unwinds its page-table references. Unmap yields but does not stop early.
- "GPU lookup by UUID failed", the "not available yet" messages and the failing-request `rmStatus` line are limited to one per call site per ten seconds (`uvm_log_slow`).

"First test boot" below is the plan the first boots followed. Their results, and the current state, are under "Status" above.

## Layout

| Path | What |
|---|---|
| `sys/nvidia_uvm_ioctl.h` | the user ABI (request numbers, parameter structures, status codes), usable from userland |
| `sys/nvidia_uvm_status.h` | the `NV_STATUS` table and the status-to-errno translation |
| `sys/nvidia_uvm_rm.h` | the kernel interface to `nvidia.ko` (entry points, shared structures, callback table) |
| `sys/uvm_hal_methods.h` | pure encoders: method stream, GPFIFO entries, fault entries, the generation table |
| `sys/uvm_pte.h` | pure page table entry encoders (MMU versions 2 and 3) and geometry |
| `sys/uvm.h` | internal types and the lock order |
| `sys/uvm_main.c` | module, device nodes, Linuxulator bridge, RM session and callbacks, tunables |
| `sys/uvm_ioctl.c` | dispatcher, fd state machine, one handler per request |
| `sys/uvm_va_space.c` | VA spaces, ranges, GPU/GPU-VA-space/channel registration, external mappings, semaphore pools |
| `sys/uvm_gpu.c` | the module-wide GPU object and its RM sequence |
| `sys/uvm_channel.c` | pushbuffer, channels, pushes, trackers |
| `sys/uvm_page_tree.c` | page trees written by the copy engine |
| `sys/uvm_faults.c` | fault buffers, interrupt control, top and bottom halves |
| `sys/uvm_mmap.c` | mmap (semaphore pools) |
| `tests/unit/` | userland unit tests of the pure parts, each written red first |
| `tests/ioctl/uvm_ioctl_test.c` | the ioctl harness of the test plan (section 1), native and Linux builds |
| `tests/amd64_cross_check.sh` | compiles every module source for amd64 with the kernel's flags |
| `man/nvidia-uvm.4` | the manual page |

## Build

```sh
make SYSDIR=/path/to/kernel/sys        # sys/nvidia-uvm.ko, -Werror
make test SYSDIR=/path/to/kernel/sys   # unit tests, then the amd64 cross-compile check
make lint                              # mandoc -T lint man/nvidia-uvm.4
```

`SYSDIR` must be the source the running kernel was built from. The module
depends on `nvidia` (with the UVM glue), `linux64elf` (arm64) or `linux`
(amd64), and `linux_common`.

The module is linked with the base toolchain (`/usr/bin/ld`, LLD) whatever
`PATH` says, and the build refuses a `.ko` that LLD did not link.  binutils'
GNU ld (in `/usr/local/bin`, first in the default `PATH`) links a module the
kernel cannot load ("symbol nvUvmInterfaceChannelDestroy undefined").
`sh tests/toolchain_test.sh` proves both with a hostile `PATH`.

## Security: RM client ownership

RM resolves a client handle with kernel privilege and never checks the caller, and handles are
predictable (the first client of a boot is `0xC1D00000`). So the module accepts a client handle
only from the process that holds the `/dev/nvidiactl` file the client was allocated on.

- **Which requests:** `UVM_REGISTER_GPU` (with a client handle or an SMC reference),
  `UVM_REGISTER_GPU_VASPACE`, `UVM_REGISTER_CHANNEL`, `UVM_MAP_EXTERNAL_ALLOCATION` and
  `UVM_ALLOC_DEVICE_P2P` carry an `rmCtrlFd` and a handle. Before any RM call the module resolves
  `rmCtrlFd` (read and ioctl rights) and asks `nvidia.ko` (`nvUvmInterfaceClientHold`, released
  with `nvUvmInterfaceClientRelease` when the request ends) whether that file owns the handle.
- **What a caller sees:** `rmStatus = NV_ERR_INSUFFICIENT_PERMISSIONS` (0x1B) for a closed fd, an
  fd without the rights, a file that is not nvidiactl, or a handle that file does not own. The
  console logs which of these it was (rate limited).
- **Dependency:** the `nvidia.ko` of nvidia-gb10-freebsd branch `agent/2373-nv-client-owner` or
  later. With an older one, `kldload nvidia-uvm` fails and the console shows
  `symbol nvUvmInterfaceClientHold undefined`.
- **Files and tests:** the rule is `sys/uvm_client_owner.h`; `tests/unit/client_owner_test.c`
  is its model test, `tests/client_owner_pins_test.sh` pins the call sites (and runs deliberately
  broken copies it must reject), and `tests/ioctl/uvm_owner_probe.c` is the two-user probe for a
  test boot (run as root with both modules loaded). Track #2373.

## Tunables

`hw.nvidia_uvm.builtin_tests` (0), `enable_va_space_mm` (1),
`gpfifo_entries` (1024), `fault_batch_count` (256), `wait_timeout_ms`
(30000), `sysmem_gpfifo` (1); diagnostics, default 0: `debug_push`, `init_probe`, `init_probe_work`, `ce_bind_subch4`. Read-only: `hw.nvidia_uvm.fatal_error`,
`hw.nvidia_uvm.gpus`. See `nvidia-uvm(4)`.

## First test boot

From a one-shot boot environment, as root:

1. `kenv hw.nvidia.registry.EnableGpuFirmware=1`
2. `kldload nvidia_gsp_ga10x_fw.ko`, then the `nvidia.ko` built from
   `nvidia-gb10-freebsd` branch `agent/uvm-phase0` (it loads `linux64` and
   `linux_common`); check `kldstat` and that `nvidia.ko` exports
   `nvUvmInterfaceSessionCreate` (`nm -D`).
3. `kldload sys/nvidia-uvm.ko`; `dmesg` shows "RM session created" and
   "loaded: /dev/nvidia-uvm and /dev/nvidia-uvm-tools"; `ls -l /dev/nvidia-uvm*`
   shows mode 0666.
4. As a user: `cc -I sys -o uvm_ioctl_test tests/ioctl/uvm_ioctl_test.c &&
   ./uvm_ioctl_test -v` (test plan section 1; needs no GPU).
5. `sysctl hw.nvidia_uvm.gpus` is empty until a process registers a GPU.
   Then, with the Linux libcuda under the Linuxulator: `deviceQuery`
   under `ktrace`, expecting the request sequence of the spec's 01 section
   6 and `pageableMemoryAccess = 0`; then `vectorAdd` (cudaMalloc +
   cudaMemcpy + a kernel). `sysctl hw.nvidia_uvm.gpus` then shows the
   GPU, its classes and zero fatal faults.
6. `kldunload nvidia-uvm` once every CUDA process has exited; it answers
   EBUSY while any node is open or any mapping of a pool or managed range is
   alive (`nvidia-uvm(4)`, UNLOADING).
7. Lifetime stress (Track #2272): `sh tests/stress/uvm_stress.sh -n 30 -j 4 -U`
   runs parallel CUDA workers with random `kill -9` and open/close/mmap churn,
   and with `-U` alternates two unload probes: with a holder keeping
   `/dev/nvidia-uvm` open (must answer "Device busy") and with none (may
   succeed; the module is then reloaded and checked).  It fails on: a new Xid,
   FATAL or panic line; a non-zero `hw.nvidia_uvm.fatal_error`; a worker that
   fails other than by `kill -9` (excused only across a successful unload); a
   worker hung 120 s after the load stops; an unload that succeeds under the
   holder; a `kldunload` failure other than "Device busy"; a failed reload;
   a churn loop failure (`churnN.fail`; the churn loops are quiesced around a
   free probe); a line with `nvidia-uvm: ... failed|error|could not` in the
   kernel log (for instance "hw.nvidia_uvm.gpus could not be registered");
   `hw.nvidia_uvm.gpus` missing after a reload (a loop reads it all through
   the run, to race the unload); no worker finishing OK; and a final idle
   unload that does not happen; a churn or sysctl loop still running 30 s
   after the load stops.  New "GPU lookup by UUID failed" lines are a WARN
   (counted against the start), not a FAIL.  `-T` self-tests the kernel log
   filter; `tests/stress/uvm_stress_selftest.sh` runs the script in fake mode
   (blind fstat, early failure, WARN lines).  `-H hold|free` picks one
   probe kind; `-h` lists every option.  The panic capture settings for a test
   BE are in `docs/panic-capture.md`.

## Licence

BSD-2-Clause, Copyright (c) 2026 REVYTECH, Inc. (`LICENSE`).
