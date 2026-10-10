# nvidia-uvm

FreeBSD glue for NVIDIA's open `nvidia-uvm` kernel module. The package is
`graphics/nvidia-uvm-kmod` in `cloudbsd-ports-overlay`. Jenkins job
`cloudbsd-ports` builds it and publishes to InternalPkg.

The version comes from `x11/nvidia-driver/Makefile.version`. Do not copy it
into the port. `tests/test_version_lock.sh` reads that file and
`graphics/nvidia-uvm-kmod/Makefile`. It does not run make. `--self-test`
checks altered copies: invalid ones must be rejected, and the unaltered
port must pass.

`tests/test_abi_initialize.sh` compiles `tests/abi_initialize.c` against the
pinned `595.99.02` headers with an amd64 compiler. `UVM_INITIALIZE` must be
`0x30000001`. `UVM_INITIALIZE_PARAMS` must be 16 bytes, aligned 8, with
`flags` at 0 and a 4-byte `rmStatus` at 8. Those are its only members. A
different ioctl number or a wider `rmStatus` must fail to compile. An extra
4-byte field still fits in the tail padding, so the member list rejects it.

`tests/test_symbol_gate.sh` runs `nm` on the FreeBSD `nv-kernel.o` from
driver `595.99.02`. Set `NV_KERNEL_O` to that object. The script hashes its
own copy of the object and of the pinned `nv_uvm_interface.c`, then reads
only those copies. The object must define no `nvUvmInterface` symbol.
Every `rm_gpu_ops_*` name in the `NV_UVM_ENABLE` arm of that file must be
a global text symbol. `src/nvidia/Makefile` beside the object sets no
`EXPORT_SYMS`. The script also hashes `nvidia-sources.mk` and
`src/common/optional-cflags.mk` from that driver tree. Neither may set
`EXPORT_SYMS`. The script also reads `/home/mlapointe/git/freebsd-ports`,
the same tree the version-lock test uses, and it does not take a
ports-root override. From that tree, `x11/nvidia-kmod`'s last `FILESDIR`
and `PATCHDIR` must point at `x11/nvidia-driver/files`, and the port must
apply `extra-patch-src-Makefile-2`. That patch's added lines must be one
`EXPORT_SYMS` assignment of the eight `nv_kthread` names. Link plan:
compile the file with `NV_UVM_ENABLE` into `nvidia-uvm.ko`, and add the
76 names to that patch in the overlay. The script does not build the module.
