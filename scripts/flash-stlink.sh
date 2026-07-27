#!/bin/bash
# Flash an ELF to the STM32MP257 over ST-LINK v3 + OpenOCD and run it on A35 core 0.
#
# Usage: flash-stlink.sh path/to/main.elf [--no-reset] [--smp] [--uart-log[=PATH]]
#
#   --no-reset          skip the board reset (board must already be parked at
#                       the "Ready" state)
#   --smp               release core 1 (resume it in its holding pen) before
#                       starting the app, for multicore projects that start
#                       core 1 themselves (VBAR_CR + core reset, or PSCI).
#                       Default is to leave core 1 halted so it cannot wake
#                       and race single-core apps.
#   --uart-log[=PATH]   after reset, wait for debug_load's "Ready" line to
#                       appear in a UART log file (default PATH:
#                       ~/minicom-devboard.log) instead of sleeping blind.
#                       Requires a terminal program appending the console UART
#                       to that file.
#
# Requires:
#  - SD card that boots TF-A BL2 -> debug_load, parking core 0 at EL3
#    ("Ready" on the UART) with MMU/caches off.
#  - OpenOCD 0.12+ (started automatically from the repo root if not running).
#
# The sequence (every step earned the hard way; see scripts/stlink/README.md):
#  1. reset run, wait for the debug_load park -> known cold-boot EL3 state.
#  2. Re-examine the A35 targets (they de-examine across srst).
#  3. `aarch64 smp off`: with SMP grouping on, "resume <addr>" redirects BOTH
#     A35 cores to the address and two cores race the app on one stack.
#  4. Halt core 1 and leave it halted: it sits in a WFE holding pen whose
#     release mailbox can hold stale garbage (backup domain survives reset),
#     and debug activity can wake it into the middle of the app image.
#  5. Load the ELF through the AXI mem-AP (like TRACE32's "MemAccess DAP") --
#     loading through the CPU can leave the image in dirty D-cache lines
#     instead of DRAM.
#  6. Run a sanitizer stub from a virgin address: full set/way
#     clean+invalidate, ic iallu, TLB invalidates, cold-boot EL1 sysregs,
#     then branch to the entry point. Without it the core intermittently
#     fetches stale instructions from the previous image.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STUB_SRC="$REPO_ROOT/scripts/stlink/sanitize_stub.S"
STUB_BIN="$REPO_ROOT/scripts/stlink/sanitize_stub.bin"
STUB_ADDR=0x9FF00000
UART_LOG="$HOME/minicom-devboard.log"
OOCD_LOG="${TMPDIR:-/tmp}/openocd-stlink.log"
TCL_PORT=6666

ELF=""
DO_RESET=1
DO_SMP=0
UART_LOG=""
for arg in "$@"; do
	case "$arg" in
	--no-reset) DO_RESET=0 ;;
	--smp) DO_SMP=1 ;;
	--uart-log) UART_LOG="$HOME/minicom-devboard.log" ;;
	--uart-log=*) UART_LOG="${arg#--uart-log=}" ;;
	*) ELF="$arg" ;;
	esac
done

if [ -z "$ELF" ] || [ ! -f "$ELF" ]; then
	echo "Usage: $0 path/to/main.elf [--no-reset] [--smp] [--uart-log[=PATH]]" >&2
	exit 1
fi
ELF="$(cd "$(dirname "$ELF")" && pwd)/$(basename "$ELF")"

oocd() {
	printf '%s\x1a' "$1" | nc -w 15 localhost $TCL_PORT | tr -d '\032'
}

die() { echo "ERROR: $1" >&2; exit 1; }

# --- Ensure the sanitizer stub is built ---
if [ ! -f "$STUB_BIN" ] || [ "$STUB_SRC" -nt "$STUB_BIN" ]; then
	echo "Building sanitizer stub..."
	aarch64-none-elf-as -o "${STUB_BIN%.bin}.o" "$STUB_SRC"
	aarch64-none-elf-objcopy -O binary "${STUB_BIN%.bin}.o" "$STUB_BIN"
fi

# --- Ensure OpenOCD is running (and responsive) ---
if ! oocd "version" 2>/dev/null | grep -q "Open On-Chip"; then
	echo "Starting OpenOCD..."
	pkill -f "openocd" 2>/dev/null || true
	sleep 1
	(cd "$REPO_ROOT" && nohup openocd > "$OOCD_LOG" 2>&1 &)
	for i in $(seq 1 20); do
		sleep 0.5
		if oocd "version" 2>/dev/null | grep -q "Open On-Chip"; then break; fi
		[ "$i" = 20 ] && die "OpenOCD did not start (see $OOCD_LOG)"
	done
fi

# --- Reset board to the parked "Ready" state ---
#
# Boot detection is done with a marker word: BL2 reloads the image from SD to
# 0x88000000 on a real boot, so a marker written there beforehand disappears
# exactly when the boot has happened. This works with no UART access.
#
# Two reset methods:
#  - "sysrst": write RCC_GRSTCSETR.SYSRST through the halted A35 (the RCC only
#    honors it from a secure master; writes via the AXI AP are silently
#    ignored). Works on any adapter, including the EV1's embedded ST-LINK,
#    whose NRST line does not reach the MPU.
#  - "srst": OpenOCD `reset run` (adapter NRST pin). Fallback if sysrst fails.
MARKER=0xdeadbeef
RCC_GRSTCSETR=0x44200400

read_image_word() {
	oocd "stm32mp25x.axi arp_examine; targets stm32mp25x.axi; read_memory 0x88000000 32 1" 2>/dev/null | tail -1
}

wait_for_boot() { # $1 = seconds to wait; returns 0 once the marker is gone
	for _ in $(seq 1 "$1"); do
		sleep 1
		W=$(read_image_word)
		case "$W" in
		"") ;;                      # AXI not readable yet (mid-reset)
		"$MARKER") ;;               # not rebooted yet
		0x*) return 0 ;;            # BL2 wrote the image back: booted
		esac
	done
	return 1
}

if [ "$DO_RESET" = 1 ]; then
	LOGSZ=$(wc -c < "$UART_LOG" 2>/dev/null || echo 0)
	echo "Resetting board..."
	oocd "reset run" > /dev/null
	READY=0
	for i in $(seq 1 20); do
		sleep 1
		if tail -c +$((LOGSZ + 1)) "$UART_LOG" 2>/dev/null | grep -q "Ready"; then READY=1; break; fi
	done
	[ "$READY" = 1 ] || die "Board did not print 'Ready' after reset (check SD card / UART log)"
	echo "Board parked at 'Ready' (${i}s)"
fi

# --- Recover targets, single-core mode, both cores halted ---
oocd "stm32mp25x.a35_0 arp_examine" > /dev/null
oocd "stm32mp25x.a35_1 arp_examine" > /dev/null
oocd "aarch64 smp off" > /dev/null
oocd "targets stm32mp25x.a35_1; halt; targets stm32mp25x.a35_0" > /dev/null
oocd "halt" > /dev/null
sleep 0.2

STATES=$(oocd "targets" | awk '/a35_[01]/ {print $NF}' | tr '\n' ' ')
[ "$STATES" = "halted halted " ] || die "A35 cores not halted (states: $STATES)"

# --- Load the image through the AXI AP (not through the CPU) ---
LOAD_OUT=$(oocd "targets stm32mp25x.axi; load_image $ELF")
oocd "targets stm32mp25x.a35_0" > /dev/null
echo "$LOAD_OUT" | grep -q "downloaded" || die "load_image failed: $LOAD_OUT"
echo "$LOAD_OUT" | grep downloaded

ENTRY=$(aarch64-none-elf-readelf -h "$ELF" | awk '/Entry point/ {print $NF}')
echo "Entry point: $ENTRY"

# --- Load sanitizer stub, pass entry point, run ---
oocd "load_image $STUB_BIN $STUB_ADDR bin" > /dev/null
oocd "reg x20 $ENTRY" > /dev/null
if [ "$DO_SMP" = 1 ]; then
	# Release core 1 back into its holding pen so the app can start it
	# (VBAR_CR + core reset does nothing to a core halted in debug state).
	oocd "targets stm32mp25x.a35_1; resume; targets stm32mp25x.a35_0" > /dev/null
fi
oocd "resume $STUB_ADDR" > /dev/null
echo "Running. UART output: tail -f $UART_LOG"
