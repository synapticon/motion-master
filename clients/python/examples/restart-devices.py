#!/usr/bin/env python3
"""Restart devices through INIT up to an AL state (POST /api/devices/restart).

The server commands each device to INIT, and then up one state at a time to --state: PRE-OP,
SAFE-OP, OP. A --state of BOOT goes from INIT to BOOT. The walk stops at the first state that a
device does not reach. A restart drops every device it restarts out of the process data exchange,
so a drive that was enabled is disabled.

Opens the fieldbus driver and scans first when needed. The script exits with 1 when a device does
not reach --state.

    python restart-devices.py
    python restart-devices.py --state pre-op --slave 2
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import STATE_NAMES, MotionMasterClient  # noqa: E402

STATES = {name.lower(): value for value, name in STATE_NAMES.items() if value != 0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", choices=STATES, default="op")
    ap.add_argument("--slave", type=int, help="one device position (default: every device)")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_scanned()

    body = {"target": STATES[args.state]}
    if args.slave:
        body["positions"] = [args.slave]
    result = client.call("restartDevices", body=body).json()
    for d in result["devices"]:
        mark = "" if d["reached"] else f"  not reached, alStatusCode 0x{d['alStatusCode']:04x}"
        print(f"  {d['slavePosition']:3d}  {STATE_NAMES.get(d['alState'], d['alState'])}{mark}")
    if not result["ok"]:
        sys.exit(f"the restart stopped at {STATE_NAMES.get(result['step'], result['step'])}")


if __name__ == "__main__":
    main()
