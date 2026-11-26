- (uvm-2272 2026-11-20) -Wcast-qual on a %p log of a volatile pointer: cast to (volatile void *), never (void *); the qualifier stays, nothing is silenced. amd64 cross-check catches it, the arm64 module build does not. On FreeBSD, sed -i needs an explicit '' suffix or it leaves a file named x-e.
- (uvm-2504 2026-11-25) A pin that only greps for a substring does not prove that a helper decides the
  path: `(void)helper(x); if (status != NV_OK) fail;` keeps the text and breaks the behaviour. Pin the
  exact control-flow line instead (`if (verdict.decision == X) {`) and show the pin can fail: build a
  mutant of the call site, prove it differs from the original with cmp, and require the pin to reject it.
  Same for the rules themselves: compile the unit test against a mutated copy of the header and require
  the test to fail, so the test cannot pass vacuously.
- (uvm-2504 2026-11-25) Read a mutable tunable once, into a local, and take the reason for a decision
  from that same value (a helper that returns the placement and its reason), never from a second read of
  the global: two reads can make the log message and the action disagree.
- (uvm-2504 2026-11-25) A scanner that walks a directory reads every file in it, including the copies of
  FreeBSD's generated headers that the kernel build leaves in sys/; lizard honours a .gitignore only
  inside the directory it scans, so the root .gitignore does not keep them out and a FreeBSD macro was
  counted as ours. Repeat the patterns in a .gitignore next to the copies. A C main() over the 100-line
  limit is best fixed by making the cases a table: one row per check, the same messages, and the row
  carries the line it is written on, so a failure still names the case. Prove it: count the checks with
  an instrumented build before and after, and diff the failing output of the mutants.
- (uvm-2444 2026-11-25) The FreeBSD ioctl word is: direction bits 29-31, size in bits 16-28,
  group byte in bits 8-15, number in bits 0-7 (`sys/ioccom.h`). A decode that compares the top 16
  bits against `IOC_VOID | group` never matches, because the group byte is below bit 16: every
  request decodes to 0 and every ioctl answers ENOTTY. Check the direction, the size, the group and
  the number separately, and round-trip every id in a unit test.
- (uvm-2444 2026-11-25) mandoc -T lint wants the OS name as a bare `.Fx` (a literal "FreeBSD" in
  running text is a style warning), and an `.Xr` to a page of this repository resolves only when
  that page is in the current directory: lint the pages from the man directory, not from the
  repository root, or every cross-reference is "referenced manual not found". In SEE ALSO the
  order is by section first, then by name.
- (uvm-2444 2026-11-25) A module directory that includes the Linuxulator headers through
  `machine/../linux/linux.h` needs the `machine` symlink that bsd.kmod.mk creates for the object
  directory (it points at ${SYSDIR}/${MACHINE}/include). It is a build product: gitignore it, or
  the tree carries a symlink to the kernel source.
- (uvm-2444 2026-11-25) A kmod's dependency set must be compared exactly, not by membership: a
  driver that depends on the Linuxulator again still contains "kernel" and "nvidia", so a
  membership check passes while every native boot drags the Linuxulator in. Read the dependency
  names from the module's `_mod_metadata_<mod>_depend_on_<dep>` symbols.
- (uvm-2444 2026-11-26) A mapping between two numbering schemes is ONE table, and the table is
  written as an X-macro list whose row macro pastes the constants from the request name:
  `UVM_REQ_ ## suffix`, `LINUX_UVM_ ## suffix`, `sizeof(UVM_ ## suffix ## _PARAMS)`.  A row then
  names its three constants and cannot be given the wrong one, the two directions are small
  lookups over the const array the list builds, and the unit test walks that same array instead
  of holding a third copy of the lists (a switch that translates both ways is 114 lines and 54
  branches, and its twin is what cpd finds).
- (uvm-2444 2026-11-26) NVIDIA's MIT `uvm_ioctl.h` is read by a generator, never by hand: the
  request names come out of it and nothing else does.  Its Linux ioctl numbers are the ones
  uvm-spec r1 01-user-abi.md section 2.1 gives (every one compares equal), but the two
  initialisation requests are not in NVIDIA's Linux interface at all, so their numbers come from
  the specification alone and have to be pinned somewhere else.
- (uvm-2444 2026-11-26) A generated file that is committed can still be generated at build time:
  the build regenerates it into a scratch file and compares it with the committed one when the
  input is available, and uses the committed one when it is not.  A build then needs no
  proprietary source, and a stale table is still caught.
- (uvm-2444 2026-11-26) `lizard -w` prints only the warnings: an empty output means the file was
  read and nothing tripped, not that nothing was read.  Run it without `-w` once to see the
  per-function table and know that the files are in the scan.
