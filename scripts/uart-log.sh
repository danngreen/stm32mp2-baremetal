#!/bin/bash
# Append a serial device's output to a log file, unbuffered.
#
# Replacement for a minicom capture session: minicom's capture buffers in ~4KB
# blocks, so low-volume console output can lag the log file by minutes. cat
# writes each read() through immediately.
#
# Usage: uart-log.sh [device] [logfile] [baud]
#   defaults: /dev/cu.usbserial-FTDXR9MP  ~/minicom-devboard.log  115200
#
# Run detached:  nohup scripts/uart-log.sh >/dev/null 2>&1 &

DEV="${1:-/dev/cu.usbserial-FTDXR9MP}"
LOG="${2:-$HOME/minicom-devboard.log}"
BAUD="${3:-115200}"

# Hold the device open on fd 3 while configuring it: on macOS the termios
# settings reset when the last file descriptor closes.
exec 3<>"$DEV" || exit 1
stty -f "$DEV" "$BAUD" cs8 -parenb -cstopb clocal -crtscts raw

echo "--- uart-log.sh: $DEV @ $BAUD, $(date) ---" >> "$LOG"
exec cat <&3 >> "$LOG"
