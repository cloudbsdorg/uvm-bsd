# nvidia-uvm

FreeBSD glue for NVIDIA's open `nvidia-uvm` kernel module. The package is
`graphics/nvidia-uvm-kmod` in `cloudbsd-ports-overlay`. Jenkins job
`cloudbsd-ports` builds it and publishes to InternalPkg.

The version comes from `x11/nvidia-driver/Makefile.version`. Do not copy it
into the port. `tests/test_version_lock.sh` reads that file and
`graphics/nvidia-uvm-kmod/Makefile`. It does not run make. `--self-test`
checks altered copies: invalid ones must be rejected, and the unaltered
port must pass.
