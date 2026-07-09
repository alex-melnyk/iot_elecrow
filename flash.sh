#!/usr/bin/env bash
#
# flash.sh — compile, flash, and monitor the Victron circular-display firmware
#            on an Elecrow CrowPanel 1.28" Rotary Display (ESP32-S3).
#
# Usage:
#   ./flash.sh            # compile + flash + monitor (default)
#   ./flash.sh compile    # compile only
#   ./flash.sh flash      # compile + flash
#   ./flash.sh monitor    # just tail the serial monitor
#
# Env overrides:
#   PORT=/dev/cu.usbmodemXXXX   # skip auto-detect
#   MON_SECS=60                 # how long `monitor` reads (default 30)
#
# Requires: arduino-cli, esp32:esp32 core, python3 + pyserial.
# One-time setup, if needed:
#   brew install arduino-cli
#   arduino-cli config add board_manager.additional_urls \
#     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
#   arduino-cli core update-index && arduino-cli core install esp32:esp32
#   arduino-cli lib install "LovyanGFX" "PubSubClient" "ArduinoJson"
#   python3 -m pip install --user pyserial

set -euo pipefail

SKETCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ESP32-S3 Dev Module with 16MB flash, OPI PSRAM, huge-app partition.
# USBMode=hwcdc + CDCOnBoot=cdc route Serial over the native USB-Serial-JTAG
# port (this board has no separate USB-UART chip).
FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=huge_app"
MON_SECS="${MON_SECS:-30}"

find_port() {
  if [[ -n "${PORT:-}" ]]; then echo "$PORT"; return; fi
  # First USB serial port (the board enumerates as an Espressif USB-JTAG unit)
  arduino-cli board list 2>/dev/null | awk '/usbmodem|USB JTAG|Espressif|serial/ {print $1; exit}'
}

do_compile() {
  echo "==> Compiling ($FQBN)"
  arduino-cli compile --fqbn "$FQBN" "$SKETCH_DIR"
}

do_flash() {
  local port; port="$(find_port)"
  [[ -n "$port" ]] || { echo "!! No board serial port found. Plug it in, or set PORT=..." >&2; exit 1; }
  echo "==> Flashing to $port"
  arduino-cli upload -p "$port" --fqbn "$FQBN" "$SKETCH_DIR"
}

do_monitor() {
  local port; port="$(find_port)"
  [[ -n "$port" ]] || { echo "!! No board serial port found." >&2; exit 1; }
  echo "==> Monitoring $port for ${MON_SECS}s (115200 baud)"
  # Native-USB CDC: read WITHOUT toggling DTR/RTS (that resets the S3 and drops the port).
  PORT="$port" MON_SECS="$MON_SECS" python3 - <<'PY'
import os, sys, time, serial
port = os.environ["PORT"]; dur = float(os.environ["MON_SECS"])
ser = serial.Serial(port, 115200, timeout=1)
start = time.time()
while time.time() - start < dur:
    line = ser.readline()
    if line:
        sys.stdout.write(line.decode(errors="replace")); sys.stdout.flush()
ser.close()
PY
}

case "${1:-all}" in
  compile) do_compile ;;
  flash)   do_compile; do_flash ;;
  monitor) do_monitor ;;
  all)     do_compile; do_flash; do_monitor ;;
  *) echo "Usage: $0 [compile|flash|monitor|all]" >&2; exit 2 ;;
esac
