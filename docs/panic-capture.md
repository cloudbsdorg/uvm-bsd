# Capturing the next panic on a test boot (Track #2272)

On 2026-11-20 07:11:29Z amp001 panicked in `nvidia-uvm`, then hung: no crash
dump, and the backtrace on the console was cut off after the first frames.
This page says why that can happen with the current settings and what a test
boot environment (BE) should carry so that the next panic leaves evidence.
All paths below are the FreeBSD base facilities; nothing here is specific to
this module.

## Why the 07:11 panic left nothing

What the running BE has (read from the machine, not assumed):

| Setting | Value | Consequence |
|---|---|---|
| `debug.debugger_on_panic` | `0` | A panic never enters ddb, so the `kdb.enter.panic` script in `/etc/ddb.conf` never runs. The script is installed and correct; it is dead. |
| `debug.trace_on_panic` | `1` | A one-frame-at-a-time backtrace goes to the console only. |
| `debug.ddb.capture.bufsize` | `49152` | If the script did run, `alltrace` on 128 CPUs would overflow a 48 KiB capture buffer. |
| `kern.msgbufsize` | `98304` | The kernel message buffer, which a textdump also saves, holds about the last 96 KiB: it wraps during a long Xid burst. |
| `rc.conf` `dumpdev` | `AUTO` | Chooses a swap partition; the BE's `dumpon -l` shows the 2 GiB `gpt/swap0`, not the large `gpt/dump0` of the test BEs. |
| `ddb_enable` | unset | `/etc/rc.d/ddb` does not load `/etc/ddb.conf` at all. |
| hardware watchdog | none (`aw_wdog` is the only driver; no `/dev/watchdog`, no IPMI device) | A hang after the panic message is permanent until someone powers the machine off. |

A full dump did work before (`/var/crash/vmcore.0`, 8 GB, 2026-11-19), so the
dump path itself is sound on this machine.  The 07:11 hang is therefore
before or during the dump: either the CPUs could not be stopped (a CPU spinning
with interrupts masked inside the RM never takes the stop IPI), or the
dump write blocked.  The NVMe dump device is not on the GPU's path, so a GPU
fault does not by itself stop a dump; a stuck CPU is the likelier cause.  We
cannot tell from here, so capture as much as possible before the dump starts.

## Test BE settings

`/etc/rc.conf` in the BE:

```
dumpdev="/dev/gpt/dump0"
ddb_enable="YES"
savecore_enable="YES"
```

`/etc/sysctl.conf` in the BE (replace the `debug.debugger_on_panic=0` line):

```
debug.debugger_on_panic=1
debug.trace_on_panic=1
debug.ddb.textdump.pending=1
kern.panic_reboot_wait_time=15
```

`/boot/loader.conf` in the BE:

```
kern.msgbufsize="1048576"
debug.ddb.capture.bufsize="4194304"
```

`/etc/ddb.conf` in the BE (one script, one line when typed; `ddb(4)` and
`textdump(4)` document the commands):

```
script lockinfo=show locks; show alllocks; show lockedvnods
script kdb.enter.panic=textdump set; capture on; show registers; bt; show allpcpu; ps; alltrace; run lockinfo; capture off; textdump dump; reset
```

What each step buys: `textdump set` makes the dump a small tar of text, so it
finishes in seconds and fits `gpt/dump0` with room to spare; `capture on` puts
everything ddb prints into the textdump (and the message buffer) rather than
only the console; `show registers` and `bt` are the faulting thread, which is
what was lost at 07:11; `alltrace` is every thread, which shows which CPU or
thread held what; `textdump dump` writes the dump from inside ddb, before the
slower normal shutdown path; `reset` reboots by PSCI even if a later step
would hang.  A full memory dump (for `kgdb`) is not taken: a textdump and a
full dump are alternatives for one panic.  To keep one full dump as well, run
a second test boot with `debug.ddb.textdump.pending=0` and the script
`kdb.enter.panic=bt; show allpcpu; ps; call doadump(0); reset`.

After the next boot: `tar -tf /var/crash/textdump.tar.last` lists
`ddb.txt` (the captured output), `msgbuf.txt`, `panic.txt` and `version.txt`;
`ddb.txt` has the backtrace.  Keep the file with the Track item.

## Checks before the test starts (no reboot)

Run these in the test BE once it is up, before loading the nvidia stack:

```
dumpon -l                              # /dev/gpt/dump0
sysctl debug.debugger_on_panic         # 1
sysctl debug.ddb.capture.bufsize       # 4194304
sysctl kern.msgbufsize                 # 1048576
sysctl debug.ddb.textdump.pending      # 1
ddb script kdb.enter.panic             # prints the script above
```

`ddb script kdb.enter.panic` printing nothing means `ddb_enable` did not take
effect: fix it before any GPU work.

## Proving the capture works (optional, one extra boot)

A capture that has never run is not a capture.  At the start of a test boot,
before the nvidia modules load, as root:

```
sysctl debug.kdb.panic=1
```

The machine must come back by itself within a minute and
`/var/crash/textdump.tar.last` must exist with a `ddb.txt` that holds a
backtrace and `alltrace`.  If it hangs instead, the stop-CPU/dump path is the
problem independent of the GPU; the console on the serial line (below) shows
where.

## If it hangs anyway

- Keep a serial console log of the whole boot (`console="efi,comconsole"`
  in `loader.conf`, the BMC serial-over-LAN session logged on another
  machine).  The panic message and the frame trace of the faulting thread are
  printed before anything can hang, and a 96 KiB message buffer is not where
  they should survive.
- There is no hardware watchdog driver in this BE, so a hang needs the BMC to
  power-cycle.  Recording this on the Track item for the platform: a
  driver for the platform's generic watchdog (if the firmware exposes one) or
  an IPMI watchdog would let `watchdogd` reset a hung machine.
- `debug.kdb.break_to_debugger` and `debug.kdb.alt_break_to_debugger` stay 0:
  the point is a debugger that runs a script by itself.
