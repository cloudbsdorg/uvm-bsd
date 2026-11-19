# Architecture: a vendor-neutral core and vendor backends

Copyright (c) 2026 REVYTECH, Inc.  SPDX-License-Identifier: BSD-2-Clause

Owner decision (Mark, 2026-11-19): the UVM driver is split into a
vendor-neutral core and a vendor backend layer, NVIDIA first.  This is our
own design, not derived from anyone's code; it is recorded in
`PROVENANCE.md` and its consistency with the specification goes through the
describer's question-and-answer loop (`uvm-qa/QUESTIONS.md`, Q-006 onward).

## The two layers

| Layer | Directory | Knows about | May include |
|---|---|---|---|
| core | `sys/core/` | processes and their address spaces, VA ranges and their types, residency and migration policy, the fault-service queue on the FreeBSD VM system, device-memory accounting, work trackers as opaque handles | FreeBSD headers and `sys/core/*.h` only; never a vendor header |
| backend | `sys/nvidia/` | the `/dev/nvidia-uvm` ioctl ABI libcuda expects, the RM kernel interface of `nvidia.ko`, the Blackwell (and later) HAL: method encoders, page-table formats, fault-buffer formats, channels and pushbuffers | the core's headers and its own vendor headers |

The core never names a GPU vendor.  A "device" is a core object with an id
(processor ids 1..256; 0 is the CPU), a set of capabilities the backend
declares at probe, and an ops table the backend fills.  Everything the core
asks a device to do goes through that table.

## The ops table (`sys/core/uvm_core_ops.h`, our own)

The backend registers one `struct uvm_backend` per vendor at module load
and one `struct uvm_device_ops` per device at probe.  The table's groups,
each with neutral argument types:

1. **Device lifetime**: probe by an opaque identity (a 16-byte id the
   backend defines), retain, release, describe (name, memory size,
   page sizes supported, maximum virtual address, fault capabilities,
   DMA window).
2. **Address spaces**: create and destroy a device address space bound to a
   core VA space; map and unmap runs of pages (`uvm_phys_desc`: a list of
   bus or device physical addresses with a page size, an aperture of
   {device-local, system, peer id} and a protection of {read, read/write,
   read/write/atomic} and a caching hint); sparse and reflected special
   mappings are capability-gated ops.
3. **Work**: a tracker is an opaque 64-bit handle the backend issues; the
   core only waits on it or orders later work after it.  Copies between
   physical descriptors (for migration) are an op with a tracker result.
4. **Faults**: the backend delivers decoded fault events to the core
   (`uvm_fault_event`: device id, address-space handle, virtual address,
   access type, fault class {replayable, non-replayable}, an opaque
   hardware token); the core's fault-service queue answers per event with
   service, replay, cancel or kill-context, and the backend turns the
   answer into hardware actions.
5. **Device memory**: allocate and free device pages in the backend's
   allocator with the core's accounting (eviction callbacks flow upward
   as core requests).
6. **User ABI**: the ioctl layer belongs to the backend (the request
   numbers, structures and status codes are the vendor's user ABI); it
   calls core operations on VA spaces and ranges and translates core
   results to the vendor's status codes.

## What stays vendor-specific, and why

- The `/dev/nvidia-uvm` request numbers, parameter structures, `NV_STATUS`
  codes and errno mapping: they are the contract with libcuda.
- The RM interface and everything it hands back (channel instance
  information, PTE values RM builds for its own allocations, fault-buffer
  registers): a second vendor has none of these.
- Page-table entry formats and method encodings: hardware.
- The division of GPU work into channels and pushbuffers: the core sees
  only trackers.

What the core cannot make neutral without a second vendor to compare with
is recorded in the report of each phase and revisited when one exists.

## The move

The present phase-1 code lives flat in `sys/`.  The split is done as
opencode rounds, each a reviewed commit:

1. the ops table header and this document (no behaviour change);
2. the core: range tree, VA space bookkeeping, range groups, the fault
   service queue, device-memory accounting skeleton, built with a check
   that no vendor header is included (`tests/core_isolation_test.sh`);
3. the NVIDIA backend behind the table: ioctl layer, RM glue, HAL, page
   trees, channels, fault decoding; the module links both.
