#!/usr/bin/env python3
"""Print the CiA 402 state of a drive with its actual and target values (GET .../cia402).

CiA 402 is the device profile for drives. The server decodes the Statusword into the state of the
profile's state machine, and reads the operation mode, the actual values, the targets and the
drive's error report.

Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
SAFEOP or OP stays there.

    python cia402-state.py
    python cia402-state.py --slave 2
"""

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
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(AlState.PREOP, positions=[args.slave])

    s = client.call("getCia402Status", path={"slavePosition": args.slave}).json()
    print(
        f"state {s['state']}, statusword 0x{s['statusword']:04X}, "
        f"controlword 0x{s['controlword']:04X}"
    )
    print(f"mode {s['modeName']} ({s['operationMode']}), error report {s['errorDescription']}")
    print(f"  position  actual {column(s['actualPosition'])}  target {column(s['targetPosition'])}")
    print(f"  velocity  actual {column(s['actualVelocity'])}  target {column(s['targetVelocity'])}")
    print(f"  torque    actual {column(s['actualTorque'])}  target {column(s['targetTorque'])}")


def column(value):
    """Return a value right-aligned in 12 characters, and "-" for a value the drive did not give."""
    return f"{'-' if value is None else value:>12}"


if __name__ == "__main__":
    run(main)
