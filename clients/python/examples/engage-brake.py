#!/usr/bin/env python3
"""Engage the brake of a SOMANET drive (POST .../brake/engage).

The server writes "engaged" to the brake state, 0x2004:07, and waits --settle. A brake engages
by spring force when its voltage goes, so there is no pull time to wait for. A brake that the
drive does not let the master control is left alone, and the state says so.
The script prints the brake state that the server reads back.

Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
SAFEOP or OP stays there.

    python engage-brake.py
    python engage-brake.py --settle 200 --slave 2
"""

import json
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import AlState, MotionMasterClient, run  # noqa: E402
from options import integer, parser  # noqa: E402


def main():
    ap = parser(__doc__)
    ap.add_argument(
        "--slave", type=integer, default=1, help="the position of the device on the bus"
    )
    ap.add_argument("--settle", type=integer, default=50, help="extra wait in ms")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(AlState.PREOP, positions=[args.slave])

    r = client.call(
        "engageBrake",
        path={"slavePosition": args.slave},
        query={"settle": args.settle},
    )
    print(json.dumps(r.json(), indent=2))


if __name__ == "__main__":
    run(main)
