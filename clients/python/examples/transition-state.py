#!/usr/bin/env python3
"""Move the devices to an EtherCAT AL state (POST /api/devices/state).

Opens the fieldbus driver and scans first when needed. The server accepts a
step up of one state at a time, and BOOT only from INIT, so the client walks
through every state between. A device that does not reach a state stops the
script with its AL status code.

    python transition-state.py --state op
    python transition-state.py --state pre-op --slave 2
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import STATE_NAMES, MotionMasterClient, run  # noqa: E402

STATES = {name.lower(): value for value, name in STATE_NAMES.items() if value != 0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", required=True, choices=STATES)
    ap.add_argument("--slave", type=int, help="one device position (default: every device)")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()

    positions = [args.slave] if args.slave else None
    client.ensure_state(STATES[args.state], positions=positions, exact=True)

    query = {"positions": str(args.slave)} if args.slave else None
    for d in client.call("getDeviceStates", query=query).json():
        print(f"  {d['slavePosition']:3d}  {STATE_NAMES[d['alState']]}")


if __name__ == "__main__":
    run(main)
