#!/usr/bin/env bash
# Build, flash, and open the serial monitor. Press Ctrl+] to leave the monitor.
# Set P1535_PORT to use a port other than the one env.sh finds.
set -euo pipefail

# shellcheck source-path=SCRIPTDIR source=env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
p1535_load_idf
cd "$P1535_DIR"

# Without sdkconfig the WiFi password is empty, and the board cannot join the network.
if ! grep -q '^CONFIG_P1535_WIFI_PASSWORD=".\+"' sdkconfig 2> /dev/null; then
  echo "No WiFi password is set. Run ./configure.sh first." >&2
  exit 1
fi

idf.py -p "$(p1535_port)" flash monitor
