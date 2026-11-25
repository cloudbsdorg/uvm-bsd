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
