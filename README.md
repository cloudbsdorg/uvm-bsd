# nvidia-uvm for FreeBSD

A clean-room, BSD-2-Clause kernel module providing `/dev/nvidia-uvm` and
`/dev/nvidia-uvm-tools`, the unified virtual memory interface NVIDIA's
CUDA user-mode driver (libcuda, driver generation 580.178.04) needs beside
`nvidia.ko`. Targets FreeBSD 16.0-CURRENT on arm64 and amd64.

It is written from a functional specification only (`uvm-spec`, revision
r1, see `.clean-room`), never from NVIDIA's Linux driver; `PROVENANCE.md`
records every source consulted. Track #2101.

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

Nothing has been loaded or booted yet: see "First test boot" below.

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
6. `kldunload nvidia-uvm` once every CUDA process has exited.

## Licence

BSD-2-Clause, Copyright (c) 2026 REVYTECH, Inc. (`LICENSE`).
