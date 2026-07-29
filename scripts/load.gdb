# Attach to a running app on A35 core 0 (OpenOCD gdb port 3333).
#
# Flash first with scripts/flash-stlink.sh, then:
#   aarch64-none-elf-gdb build/main.elf -x ../scripts/load.gdb
#
# Do NOT use gdb "load" + "continue" to flash: it goes through the CPU
# (dirty-cache hazard), skips the cache-sanitizer stub, and with SMP
# grouping can send both A35 cores into the image. See
# scripts/stlink/README.md.
target extended-remote localhost:3333
monitor halt
