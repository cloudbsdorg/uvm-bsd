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
| `UVM_INITIALIZE`, `UVM_MM_INITIALIZE`, teardown | done; the secondary fd answers `NV_WARN_NOTHING_TO_DO` in this phase (no managed memory to protect at exit) |
| GPU registration through RM | done: device, address space, fault buffers, interrupt ownership, PMA object, copy-engine channels; Blackwell GB20x has the HAL, other generations are refused at registration |
| GPU VA space registration (page directory hand-over) | done, page tables in system memory written by the copy engine |
| Channel registration with resource mapping | done |
| External ranges, `cudaMalloc` mappings, sparse mappings, unmap, free, validate | done |
| Semaphore pools with mmap | done |
| Dynamic-parallelism regions | done |
| Replayable faults | fetched, attributed and cancelled (no managed memory yet); non-replayable faults reported to RM |
| Managed memory, policies, migration, range groups | later phase: requests on non-managed addresses answer `NV_ERR_INVALID_ADDRESS` as the spec says; groups are bookkeeping |
| Peer access, device P2P, access counters, tools trackers | later phases: `NV_ERR_NOT_SUPPORTED`, logged once |

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

## Tunables

`hw.nvidia_uvm.builtin_tests` (0), `enable_va_space_mm` (1),
`gpfifo_entries` (1024), `fault_batch_count` (256), `wait_timeout_ms`
(30000), `sysmem_gpfifo` (1); diagnostics, default 0: `debug_push`, `init_probe`, `ce_bind_subch4`. Read-only: `hw.nvidia_uvm.fatal_error`,
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
