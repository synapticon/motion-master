#!/usr/bin/env python3
"""Move the devices to an EtherCAT AL state.

Opens the fieldbus driver and scans first when needed. The server walks each
device through every state between, one legal transition at a time
(POST /api/devices/state/transition). A device that does not reach the state
stops the script with its AL status code.

    python transition-state.py --state op
    python transition-state.py --state preop --slave 2
"""

import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import AlState, MotionMasterClient, run  # noqa: E402
from options import AL_STATES, integer, parser  # noqa: E402


def main():
    ap = parser(__doc__)
    ap.add_argument(
        "--state", required=True, choices=AL_STATES, help="the AL state to move the devices to"
    )
    ap.add_argument("--slave", type=integer, help="one device position (default: every device)")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()

    positions = [args.slave] if args.slave else None
    client.ensure_state(AL_STATES[args.state], positions=positions, exact=True)

    query = {"positions": str(args.slave)} if args.slave else None
    for d in client.call("getDeviceStates", query=query).json():
        print(f"  {d['slavePosition']:3d}  {AlState(d['alState']).name}")


if __name__ == "__main__":
    run(main)
