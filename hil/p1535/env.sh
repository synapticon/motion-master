#!/usr/bin/env bash
# Sourced by setup.sh, configure.sh and run.sh. Pins the ESP-IDF version, finds the
# installation, and finds the serial port of the board.

# The 5.x line keeps the driver and esp_netif headers this firmware includes.
P1535_IDF_VERSION="v5.5.6"
P1535_IDF_DIR="${IDF_PATH:-$HOME/esp/esp-idf}"
P1535_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Load the ESP-IDF environment, unless the shell already has it.
p1535_load_idf() {
  if command -v idf.py > /dev/null; then
    return 0
  fi
  if [[ ! -f "$P1535_IDF_DIR/export.sh" ]]; then
    echo "ESP-IDF is not installed in $P1535_IDF_DIR. Run ./setup.sh first." >&2
    return 1
  fi
  # export.sh reads variables that may be unset, so set -u of the caller must not apply to it.
  local nounset=0
  if [[ $- == *u* ]]; then
    nounset=1
  fi
  set +u
  # shellcheck source=/dev/null
  . "$P1535_IDF_DIR/export.sh" > /dev/null
  if ((nounset)); then
    set -u
  fi
}

# Print the serial port: $P1535_PORT if set, else the first CP210x bridge, else /dev/ttyUSB0.
p1535_port() {
  if [[ -n "${P1535_PORT:-}" ]]; then
    echo "$P1535_PORT"
    return
  fi
  local port
  for port in /dev/serial/by-id/*CP210*; do
    if [[ -e "$port" ]]; then
      echo "$port"
      return
    fi
  done
  echo /dev/ttyUSB0
}
