#!/usr/bin/env bash
# Open the ESP-IDF configuration menu. The WiFi and address settings are under
# "P1535 remote control".
set -euo pipefail

# shellcheck source-path=SCRIPTDIR source=env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
p1535_load_idf
cd "$P1535_DIR"
idf.py menuconfig
