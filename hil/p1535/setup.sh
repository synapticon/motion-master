#!/usr/bin/env bash
# One-time setup of a computer that flashes the board: ESP-IDF and serial port access.
# Safe to run again; each step skips what is already done.
set -euo pipefail

# shellcheck source-path=SCRIPTDIR source=env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

if [[ -f "$P1535_IDF_DIR/export.sh" ]]; then
  echo "ESP-IDF is already in $P1535_IDF_DIR."
else
  echo "Installing ESP-IDF $P1535_IDF_VERSION in $P1535_IDF_DIR."
  mkdir -p "$(dirname "$P1535_IDF_DIR")"
  git clone --depth 1 --branch "$P1535_IDF_VERSION" --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git "$P1535_IDF_DIR"
fi
"$P1535_IDF_DIR/install.sh" esp32

if id -nG | grep -qw dialout; then
  echo "$USER is in the dialout group."
else
  echo "Adding $USER to the dialout group. It takes effect at the next login."
  sudo usermod -aG dialout "$USER"
fi

# Give access to the port now, so the first flash does not wait for a new login. The grant
# ends when the board is unplugged.
port="$(p1535_port)"
if [[ -e "$port" ]]; then
  sudo setfacl -m "u:$USER:rw" "$(readlink -f "$port")"
  echo "Access granted to $port."
else
  echo "No board found. Connect it before the first flash."
fi
