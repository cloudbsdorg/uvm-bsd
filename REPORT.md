# Track #2444 pass B: nvidia-uvm.ko is the native driver, the Linux ioctl bridge is optional

Round report, worktree `wt-uvm-2444`, branch `agent/2444-native-uvm`, 2026-11-25.
Everything is staged (`git status` shows only staged changes); nothing is committed.

The round had two halves: keep pass A's split and finish it, and rename "no video memory"
to "unified memory" everywhere. Pass A had left the tree red; the fixes below are the
substantive part of this round.

## What pass A had already done

| Item | What pass A had done | What this round added |
|---|---|---|
| 1. code | `sys/Makefile` exports `uvm_bridge_fd_is_ours` and dropped the Linuxulator `MODULE_DEPEND`; `sys/uvm_main.c` lost the Linuxulator handler, the Linux headers and the `MODULE_DEPEND` on `linux64elf`/`linux_common`; `sys/uvm_bridge.[ch]` (new) is the driver side of the bridge interface; `linux/` (new) is the optional bridge module with the Linux request numbers and their translation; `sys/nvidia_uvm_ioctl.h` gave the driver its own request numbers (`UVM_REQ_*`) and its own request word; `tests/kmod_deps_test.sh` (new) is the static load check | the driver built with the old frame buffer names still in `sys/uvm_channel.c` (build failed); the new request numbering broke `tests/unit/abi_layout_test.c` and the three ioctl harnesses; the native decode was broken (see below); the request table could have a hole; the new pins, the request table rule and the unit test for the numbering are new |
| 2. language | nothing: `README.md`, `man/nvidia-uvm.4` and the comments in `sys/` were untouched | all of it (see "the language") |
| 3. install | nothing: `scripts/make.sh` installed one module and one man page | `make install` installs both modules and both man pages; the bridge is installed, never loaded |
| 4. "unified memory" | the identifiers in `sys/uvm_fb_state.h` (`UVM_FB_UNIFIED`, `UVM_FB_DEDICATED`, `UVM_FB_PMA_UNIFIED`, `UVM_FB_GPFIFO_*`, `uvm_fb_is_unified()`) and the wording in `sys/uvm_gpu.c`, `sys/uvm_ioctl.c` | the rest of the tree: the kernel messages, the comments, the tests and their names, `README.md`, `man/nvidia-uvm.4`, the wording of `PROVENANCE.md` going forward |

## 1. The code: the native driver and the optional bridge

- `nvidia-uvm.ko` declares exactly `{kernel, nvidia}` and imports no Linux symbol
  (`nm -u` shows none). The Linuxulator dependency is gone from `sys/uvm_main.c`.
- The native ioctl encoding is the primary interface. `sys/nvidia_uvm_ioctl.h` holds the
  driver's own request numbers (`enum uvm_request_id`, dense and stable, `UVM_REQ_COUNT` is
  the number of ids) and the request word: the void direction, a zero size, the group byte
  `U` (0x55) and the request number in the low eight bits.
- The Linux ioctl bridge moved out of the driver: `linux/uvm_linux_bridge.c` holds
  `uvm_linux_ioctl`, the raw Linux request numbers (`linux/nvidia_uvm_linux_ioctl.h`, the
  numbers of uvm-spec r1 section 2.1) and the Giant handling. The module is
  `nvidia_uvm_linux.ko`; it depends on the driver, on the 64-bit Linuxulator (`linux64elf`
  on arm64, `linux64` on amd64) and on `linux_common`, and it registers the handler when it
  is loaded.
- The driver exports one function to the bridge, `uvm_bridge_fd_is_ours()`, so that the
  bridge can answer `ENOIOCTL` for a descriptor that is not one of the driver's two nodes.

**A bug in pass A's work, found and fixed.** `uvm_ioctl_native_decode()` compared the top
16 bits of the request word against `IOC_VOID | group`, but the group byte is below bit 16:
the comparison never matched, every native request decoded to 0 and every ioctl would have
answered `ENOTTY`. The decode now checks the direction (bits 29-31), the size (bits 16-28),
the group (bits 8-15) and the number (bits 0-7) separately.
`tests/unit/linux_bridge_test.c` caught it and now round-trips every request id.

**A hole in the hand-written request table.** The table in `sys/uvm_ioctl.c` is written by
hand, and a missing id answers `ENOSYS` for a request that exists with nothing to show it.
`sys/uvm_requests.h` (new) holds the rule as a pure function, `sys/uvm_ioctl.c` asks it
through `uvm_requests_dense()`, and `uvm_load()` refuses to load when the table does not
cover every id exactly once.

**The test that fails before.** `tests/kmod_deps_test.sh` (pass A's file, kept) compares the
dependency sets exactly, not by membership: the module before this change declared
`{kernel, nvidia, linux64elf, linux_common}` and the check refuses it. It also checks that
the driver imports no Linux symbol, that every undefined symbol of both modules resolves in
the kernel or in a declared dependency, and that the driver exports the bridge's entry
point. Each check is run once against a planted input it must refuse.

## 2. The language

- `README.md`: the headline is now the native FreeBSD driver ("a native caller (our own
  libcuda, Track #2428) talks to it with the driver's own request numbers, and it depends on
  no Linux module at all"). "Linux CUDA works through the Linuxulator" is gone from the
  status list; what still runs today is stated under a new section **"Linux binaries
  (optional)"**, with the `kldload` of the bridge and the measured results (cuInit, the
  vectorAdd kernel, llama.cpp b12531) kept as they were measured.
- `man/nvidia-uvm.4`: the driver is described as the native driver; the Linuxulator bridge
  is the optional module for third-party Linux CUDA binaries; the Giant paragraph now says
  that releasing Giant is the bridge's business, not the driver's.
- `man/nvidia-uvm-linux.4` (new): the manual page of the bridge module. `mandoc -T lint`
  is clean on both pages.
- Comments in `sys/`: `sys/uvm.h` and `sys/uvm_main.c` say what the module is (the native
  FreeBSD driver) and where the Linux numbers live (the optional bridge module).

## 3. The install

- `scripts/make.sh` stages four files: `/boot/modules/nvidia-uvm.ko`,
  `/boot/modules/nvidia_uvm_linux.ko`, `$PREFIX/share/man/man4/nvidia-uvm.4` and
  `$PREFIX/share/man/man4/nvidia-uvm-linux.4`. The manifest, the modes, the ownership and
  the deinstall follow the same rules as before (`allowed_path` knows the two new paths).
- The installer never loads anything: the bridge is installed and left alone. `kldxref` is
  still the only post-install step.
- `Makefile`: `make build` builds both modules, `make test` builds first (the module tests
  read the built modules), `make lint` lints both pages from the man directory (mandoc
  resolves a cross-reference to a page of this repository only when it is in the current
  directory).
- The test seam `MK_KO_LINUX` was added next to `MK_KO`; without it the planted driver
  module stands in for the bridge. Both seams are documented in the script header, and the
  header check in `tests/make_install_test.sh` knows about them.

## 4. "Unified memory"

Mark, 2026-11-25: "those other terms make it clear". A GPU like the GB10 has no dedicated
memory; it uses shared system memory.

- The identifiers: `UVM_FB_ABSENT` -> `UVM_FB_UNIFIED`, `UVM_FB_USABLE` -> `UVM_FB_DEDICATED`,
  `UVM_FB_PMA_ABSENT` -> `UVM_FB_PMA_UNIFIED`, `UVM_FB_GPFIFO_VIDEO` -> `UVM_FB_GPFIFO_DEDICATED`,
  `UVM_FB_GPFIFO_SYSMEM_NO_VIDEO` -> `UVM_FB_GPFIFO_SYSMEM_UNIFIED`,
  `uvm_fb_no_video_memory()` -> `uvm_fb_is_unified()`.
- The first use reads: "unified memory: the GPU has no dedicated memory and uses shared
  system memory". A GPU with memory of its own is "dedicated memory".
- The kernel message on registration reads
  `"nvidia-uvm: NVIDIA GB10: unified memory (no dedicated memory); GPU memory is shared system memory"`.
- The words are gone from the sources, the comments, the tests, the test names, `README.md`
  and `man/nvidia-uvm.4`. `PROVENANCE.md` keeps the wording of the earlier entries (it
  records what was written then) and says in the new entry that the names changed; the
  first mention of the old names carries the new name in brackets.
- NVIDIA's own RM names are kept where the code calls RM: `bZeroFb` and
  `PDB_PROP_GPU_ZERO_FB`, with a comment in `sys/uvm_fb_state.h` that RM calls this "zero
  framebuffer".

## The tests and the build

| What | Result |
|---|---|
| `sh tests/run_unit_tests.sh` | 615 ok, 0 FAIL, exit 0 |
| `sh tests/make_install_test.sh` | all checks passed, exit 0 |
| `sh tests/kmod_deps_test.sh` | PASS (11 checks, 3 of them controls) |
| `sh tests/amd64_cross_check.sh /usr/src/sys` | 14 sources ok (13 of the driver, 1 of the bridge) |
| `make build SYSDIR=/usr/src/sys` | both modules, arm64, `-Werror`, exit 0 |
| `readelf -p .comment` on both `.ko` | `Linker: LLD 21.1.8` |
| `mandoc -T lint` on both pages | clean (exit 0) |
| `nm` on the modules | driver: `{kernel, nvidia}`; bridge: `{kernel, linux64elf, linux_common, nvidia_uvm}` |

Nothing was loaded into the kernel and nothing was rebooted: this round is a source and
build round.

The tests that had to change because of the split, and why:

- `tests/unit/abi_layout_test.c`: the request-number assertions were the Linux numbers;
  they are now the driver's own numbers, and the round-trip check walks every id.
- `tests/unit/fb_state_test.c`: the renamed identifiers and the renamed wording; the case
  table `novid_cases` is now `unified_cases`.
- `tests/ioctl/uvm_ioctl_test.c`, `uvm_owner_race.c`, `uvm_owner_probe.c`: they speak the
  driver's request numbers now. The harness still builds for Linux, where it speaks the
  Linux numbers through the bridge header.
- `tests/run_unit_tests.sh`: the pins follow the renamed identifiers, and two pins are new:
  the driver must depend on no Linuxulator module and the bridge must register the handler.
  Each is shown to be able to fail (a mutant that adds the dependency back).
- `tests/giant_yield_log_test.sh`: the Giant pin followed `uvm_linux_ioctl` into the bridge
  module; `tests/pins_lib.sh` can now copy an extra source directory into the scratch
  directory of a mutation self-test.
- `tests/make_install_test.sh`: the install writes four files now.

## Honcho: the conclusions I read first (top 5 for the topic)

The hits are information only; they did not add, skip or change a step. No hit contradicts
a step of this task, and two of them were used as they are:

1. `LESSON [uvm-bsd-build]`: the module can be compiled on this host with no root, no
   kldload and no reboot: `make build` (SYSDIR=/usr/src/sys, aarch64 FreeBSD 16.0-CURRENT,
   -Werror) builds sys/nvidia-uvm.ko, and `sh tests/amd64_cross_check.sh /usr/src/sys`
   compiles every module source for amd64. Used as the build recipe of this round (the
   bridge module was added to both).
2. `LESSON [uvm-install-build-seam]`: uvm-bsd install always invokes the incremental module
   build when MK_KO is unset or empty; an existing sys/nvidia-uvm.ko is not sufficient; a
   non-empty MK_KO names a planted module and is the only build-skipping path. Kept: the
   install still builds, and `MK_KO_LINUX` was added beside `MK_KO` for the bridge.
3. `LESSON [freebsd-arm64-module-deps]`: on FreeBSD arm64 there is no io module, and the
   64-bit Linuxulator registers `MODULE_VERSION(linux64elf)`; 'linux' is the 32-bit
   Linuxulator on amd64; a kmod with `MODULE_DEPEND(x, linux)` links fine but cannot kldload.
   Used: the bridge depends on `linux64elf` on arm64 and `linux64` on amd64, and the
   dependency test reads the names from the module's own metadata symbols.
4. `LESSON [gb10-arm64-deps]`: the same facts for the nvidia kmod port, and the note that
   linux64.ko needs linux_common.ko. Used: the bridge declares both.
5. `LESSON [nvidia-modeset-arm64-module-deps]`: nvidia-modeset.ko on arm64 declares
   `MODULE_DEPEND(nvidia_modeset, kernel), (linux64elf), (linux_common) and (nvidia)`, and a
   dependency-set check must allow 'kernel' optionally while requiring the others exactly.
   Used: the driver's own set is compared exactly against `{kernel, nvidia}`, with the
   kernel allowed as the kernel is not a module.

Nothing in the hits conflicts with this task; the only point of contact is the name of the
64-bit Linuxulator, which the code uses as the hits describe.

## Honcho: the lessons this round recorded

Six conclusions were created in the session `lessons-native-nvidia` (observer and target
`mlapointe`):

- `LESSON [uvm-ioctl-word-layout]`: the layout of the FreeBSD ioctl request word, and why a
  decode that compares the top 16 bits against `IOC_VOID|group` never matches.
- `LESSON [mandoc-lint-local-xr]`: the OS name must be a bare `.Fx`, and a cross-reference
  to a page of this repository resolves only when that page is in the current directory.
- `LESSON [kmod-machine-symlink]`: the `machine` symlink a module directory needs for the
  Linuxulator headers is a build product and must be gitignored.
- `LESSON [kmod-deps-exact-set]`: a dependency set must be compared exactly, not by
  membership.
- `LESSON [uvm-request-table-density]`: a hand-written request table must be checked for
  density before the module serves anything.
- `LESSON [uvm-native-driver-split]`: the driver is native and the bridge is optional, with
  the dependency names of each.

The same lessons are in `LESSONS-PENDING.md` in the repository.

## What is not done

- Nothing was loaded into the kernel: `kldload nvidia-uvm.ko`, `kldload
  nvidia_uvm_linux.ko` and a test boot with the GB10 are still to be done on the hardware.
- The port in the CloudBSD ports overlay (`graphics/nvidia-uvm-kmod`) does not know about
  the second module yet.
- `sys/uvm_main.c` still declares `MODULE_DEPEND(nvidia_uvm, nvidia, 2, 2, 2)`: the driver's
  module version is unchanged, so the bridge's dependency range (1, 1, 1) is the one that
  will need a bump when the bridge interface changes.
