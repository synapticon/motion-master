#!/usr/bin/env python3
"""Blink the LEDs of a device, so that someone at the machine can find it (device-locate).

The LEDs blink for --duration seconds and stop at the end of the run, or at once on Ctrl-C. Only
a fieldbus driver that can blink the LEDs offers the procedure, which today is SPoE. Over SOEM the
device has no device-locate procedure, and the server refuses the run with 404.

Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
SAFEOP or OP stays there.

    python device-locate.py
    python device-locate.py --duration 30 --slave 2
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

PROCEDURE = "device-locate"


def main():
    ap = parser(__doc__)
    ap.add_argument(
        "--slave", type=integer, default=1, help="the position of the device on the bus"
    )
    ap.add_argument("--duration", type=integer, default=10, help="seconds, 1 to 3600")
    args = ap.parse_args()
    body = {"duration": args.duration}

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(AlState.PREOP, positions=[args.slave])

    run_to_end(client, args.slave, PROCEDURE, body)


if __name__ == "__main__":
    run(main)
