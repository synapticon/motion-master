#!/usr/bin/env python3
"""Run offset detection on a SOMANET drive (POST .../procedures/offset-detection).

Offset detection runs every measurement a motor needs, in the order they depend on each other:
open phase detection, phase resistance, phase inductance, pole pair detection, motor phase order
detection and commutation offset measurement. The script prints each step when its status
changes, with the value it produced.

This turns the rotor, and it releases the brake for part of the run. The shaft must be free, and
a load on a vertical axis must be supported first. The server lists every caveat in the
descriptor of the procedure: GET /api/devices/{slavePosition}/procedures.

Opens the fieldbus driver, scans and moves the device to OP first when needed, because the drive
enables only while it exchanges process data. Ctrl-C cancels the run. The new phase order and
commutation offset are not saved to flash. Run the store-parameters procedure to keep them across
a power cycle.

    python offset-detection.py
    python offset-detection.py --slave 2
"""

import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import (  # noqa: E402
    AlState,
    MotionMasterClient,
    run,
)
from options import integer, parser  # noqa: E402
from procedures import run_to_end  # noqa: E402

PROCEDURE = "offset-detection"


def main():
    ap = parser(__doc__)
    ap.add_argument(
        "--slave", type=integer, default=1, help="the position of the device on the bus"
    )
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(AlState.OP, positions=[args.slave])

    run_to_end(client, args.slave, PROCEDURE)


if __name__ == "__main__":
    run(main)
