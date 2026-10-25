#!/usr/bin/env python3
"""Engage the brake of a SOMANET drive (POST .../brake/engage).

The server writes "engaged" to the brake state, 0x2004:07, and waits --settle. A brake engages
by spring force when its voltage goes, so there is no pull time to wait for. A brake that the
drive does not let the master control is left alone, and the state says so.
The script prints the brake state that the server reads back.

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. A device in
SAFE-OP or OP stays there.

    python engage-brake.py
    python engage-brake.py --settle 200 --slave 2
"""

import argparse
import json
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import PRE_OP, MotionMasterClient, run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--settle", type=int, default=50, help="extra wait in ms (default: 50)")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(PRE_OP, positions=[args.slave])

    r = client.call(
        "engageBrake",
        path={"slavePosition": args.slave},
        query={"settle": args.settle},
        check=False,
    )
    if r.status_code != 200:
        sys.exit(r.json().get("error", r.text))
    print(json.dumps(r.json(), indent=2))


if __name__ == "__main__":
    run(main)
