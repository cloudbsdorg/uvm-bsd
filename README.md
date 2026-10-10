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
