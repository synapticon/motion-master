#!/usr/bin/env python3
"""Set the position, velocity or torque target of a CiA 402 drive (POST .../cia402/target).

The value is in the units of the object: 0x607A for position, 0x60FF for velocity, and 0x6071
for torque, in per mille of rated torque. Choose the target that matches the operation mode of
the drive. A drive that is enabled in that mode moves to the target at once. The server writes
the target in the process data when the device exchanges it, and with an SDO write otherwise.
cia402-state.py prints the targets and the actual values.

Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
SAFEOP or OP stays there.

    python set-target.py --target velocity --value 1000
    python set-target.py --target position --value 0 --slave 2
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
    ap.add_argument(
        "--target",
        choices=["position", "velocity", "torque"],
        required=True,
        help="the target to set",
    )
    ap.add_argument(
        "--value", type=integer, required=True, help="signed, in the units of the object"
    )
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(AlState.PREOP, positions=[args.slave])

    client.call(
        "setCia402Target",
        path={"slavePosition": args.slave},
        body={"target": args.target, "value": args.value},
    )
    print(f"slave {args.slave} {args.target} target = {args.value}")


if __name__ == "__main__":
    run(main)
