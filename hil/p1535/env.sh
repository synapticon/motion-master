#!/usr/bin/env bash
# Sourced by setup.sh, configure.sh and run.sh. Pins the ESP-IDF version, finds the
# installation, and finds the serial port of the board.

# The 5.x line keeps the driver and esp_netif headers this firmware includes.
P1535_IDF_VERSION="v5.5.6"
P1535_IDF_DIR="${IDF_PATH:-$HOME/esp/esp-idf}"
# configure.sh and run.sh use it. shellcheck reads this file on its own and cannot see that.
# shellcheck disable=SC2034
P1535_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The newest Python that tools/detect_python.sh of this ESP-IDF version names. On Python 3.15
# (Fedora 45) the install fails: tree_sitter has no wheel for it, and pip cannot build one.
P1535_PYTHON="python3.13"
P1535_PYTHON_SHIM="$HOME/.espressif/p1535-python"

# Put $P1535_PYTHON first on PATH as python3 when the system python3 is newer. ESP-IDF takes
# the first python3 on PATH, and install.sh and export.sh must take the same one.
p1535_use_supported_python() {
  if python3 -c 'import sys; sys.exit(sys.version_info[:2] > (3, 13))' 2> /dev/null; then
    return 0
  fi
  if ! command -v "$P1535_PYTHON" > /dev/null; then
    echo "python3 is newer than ESP-IDF $P1535_IDF_VERSION supports, and $P1535_PYTHON is" \
      "not installed. Run ./setup.sh." >&2
    return 1
  fi
  mkdir -p "$P1535_PYTHON_SHIM"
  ln -sf "$(command -v "$P1535_PYTHON")" "$P1535_PYTHON_SHIM/python3"
  ln -sf "$(command -v "$P1535_PYTHON")" "$P1535_PYTHON_SHIM/python"
  export PATH="$P1535_PYTHON_SHIM:$PATH"
}

# Load the ESP-IDF environment, unless the shell already has it.
p1535_load_idf() {
  if command -v idf.py > /dev/null; then
    return 0
  fi
  if [[ ! -f "$P1535_IDF_DIR/export.sh" ]]; then
    echo "ESP-IDF is not installed in $P1535_IDF_DIR. Run ./setup.sh first." >&2
    return 1
  fi
  p1535_use_supported_python
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
