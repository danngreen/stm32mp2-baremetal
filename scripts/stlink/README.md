# Flashing over ST-LINK v3 + OpenOCD

Use `scripts/flash-stlink.sh` to flash using ST-LINK and OpenOCD:

```sh
cd ctest
make
../scripts/flash-stlink.sh build/main.elf
```

It starts OpenOCD automatically (from the repo root, using `openocd.cfg`)
if it isn't already running. A full cycle (reset -> TF-A boot -> load run)
takes about 5 seconds.

## Requirements

- An ST-LINK v3: either an external one on a MIPI-10 header (for a custom
  board, make sure nRESET is on pin 10), or the EV1's embedded ST-LINK over
  its USB jack. Both are tested.
- OpenOCD 0.12+.
- SD card that boots TF-A BL2 with `debug_load` spinning. This parks core 0 at
  EL3 with MMU/caches off and prints `Ready` on the UART. 

## Options

- `--no-reset`: skip the reset (board must already be parked at `Ready`).
  Don't use after an app has run.
- `--smp`: resumes core 1 (back into its holding pen) just before starting
  the app. Default (no flag) keeps core 1 halted so it cannot wake and race
  single-core apps.

  Multicore apps normally do NOT need this flag: `multicore_smp/psci.cc`
  (`unhalt_cpu1`) shows the robust app-side pattern — before releasing core 1
  via `CA35SYSCFG->VBAR_CR` + core reset, check `EDPRSR.HALTED` and restart a
  debug-halted core 1 through its CTI. The CoreSight debug components are
  software-visible on the system bus (CPU1 external debug at `0x4A310000`,
  its CTI at `0x4A320000`; same registers the debugger reaches via AP0 at
  `0x8031xxxx`/`0x8032xxxx`). A core-1 release that skips this hangs in
  `start_cpu1` whenever a debugger or bootloader left core 1 halted, since
  the RCC core-reset request never completes for a debug-halted core. Use
  `--smp` only for multicore apps that lack this pattern.

Debugging tip: with SMP grouping off, per-target `reg pc` on `a35_1` can
misreport the current core's cached registers. To see where a core is
*really* executing (without halting it), read its `EDPCSR` PC-sample
register: `stm32mp25x.axi read_memory 0x4A3100A0 32 1` (core 1) or
`0x4A2100A0` (core 0).
- `--uart-log[=PATH]`: sanity-check that debug_load's `Ready` banner appeared
  in a UART log file (default `~/minicom-devboard.log`) after reset. Purely
  optional: boot progress is detected without any UART, by writing a marker
  word to 0x88000000 and waiting for BL2 to overwrite it when it reloads the
  image from SD.

## How the reset works

The script prefers a **software system reset**: writing
`RCC_GRSTCSETR.SYSRST` (0x44200400) *through the halted A35 core 0*. The RCC
only honors this write from a secure master — writes via the AXI mem-AP are
silently ignored — and the halted core at EL3 is one. This works on any
adapter, including the EV1's embedded ST-LINK, whose NRST line does not reach
the MPU (adapter `reset run` silently fails to reboot the EV1). If the sysrst
doesn't take (e.g. the core is wedged in a state where it can't perform the
write), the script falls back to adapter srst via `reset run`.


## Why the naive gdb flow does not work

`target extended-remote :3333` + `load` + `continue` fails in several ways:

1. **SMP resume redirects both cores.** The board config groups the two
   A35s (`target smp`). OpenOCD's SMP `resume <addr>` (and gdb resume
   after setting `$pc`) redirects *every* core in the group to that
   address, so core 0 and core 1 race through the app on the same stack.
   Core 1 never ran TF-A: its `CPTR_EL3`, `ELR_EL3`, `SPSR_EL3` are reset
   garbage, which produced the classic "FP trap at `0x880000c0`" and
   "eret to garbage address" crashes. Fix: `aarch64 smp off`.

2. **Core 1 escapes its holding pen.** Core 1 waits in a WFE pen (in
   SYSRAM) polling a release mailbox. Backup-domain registers survive
   reset, and debug activity generates WFE wake events, so core 1 can
   wake mid-session and jump into the app image. Fix: halt core 1 and
   leave it halted. (Multicore projects must resume/release it
   deliberately.)

3. **Loading through the CPU can leave the image in dirty D-cache lines**
   instead of DRAM (observed directly: after an invalidate-without-clean,
   parts of the image reverted to the previous contents). TRACE32 loads
   via `SYStem.MemAccess DAP` for the same reason. Fix: load through the
   AXI mem-AP (`targets stm32mp25x.axi; load_image ...`).

4. **Stale instruction fetch.** Even from the pristine parked state, with
   `SCTLR_EL3.{M,C,I}=0 `, the core intermittently *executes* instructions
   from the previously loaded image while data reads of the same
   addresses return the new image. A ranged `dc civac` + `ic iallu` was
   not sufficient. Fix: `scripts/stlink/sanitize_stub.S`, run from a
   virgin address: full set/way clean+invalidate of all cache levels,
   `ic iallu`, `tlbi alle3/alle1`, zeroed `SCTLR_EL1`/`VBAR_EL1`, then
   `br` to the entry point (in `x20`).

5. **`reset run` de-examines the A35 targets** (srst while the debug
   domain is down). Every cycle re-runs `arp_examine` on both cores
   afterward. This is why OpenOCD sometimes won't connect after
   `monitor reset`.

## Debugging with gdb

Flash first with the script, then attach without loading:

```sh
aarch64-none-elf-gdb build/main.elf -x ../scripts/load.gdb
```

`load.gdb` only connects and halts. From there, use normal gdb:
breakpoints (`hbreak` — flash nothing so sw breaks are fine too),
`info registers`, `x/...`, `continue`, `monitor <openocd cmd>`.

To restart the app: re-run `flash-stlink.sh` (it resets the board), then
re-attach gdb. Avoid gdb `load` + `continue` for the reasons above.

Note: some project directories contain a `.gdbinit` with the old
`target/load` flow; prefer launching gdb with `-nx` or from the repo root
so it doesn't auto-run.

## TCL one-liners

The script talks to OpenOCD's TCL port (6666). Handy for ad-hoc pokes:

```sh
oocd() { printf '%s\x1a' "$1" | nc -w 5 localhost 6666 | tr -d '\032'; }
oocd "targets"                                   # state of all cores
oocd "halt; reg pc"                              # where is core 0?
oocd "stm32mp25x.axi read_memory 0x88000000 32 8"  # peek DRAM via AXI
```

Note that each `nc` invocation is a fresh TCL session, but `targets <t>`
changes the *global* current target, and only the last command's output
in a compound `a; b; c` line is returned.
