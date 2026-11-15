#!/usr/bin/env python3
"""Restore the default parameter values of a device (procedure restore-default-parameters).

The procedure writes the "load" signature to 0x1011 for the group you choose, and polls the
device until it reports the restore complete. The live values of that group are replaced, so
anything not stored is gone. The defaults go into volatile memory: run store-parameters.py to keep
them, and some devices apply them only after a reset or a power cycle. Ctrl-C stops the wait, not
the restore, because the device already has the command.

Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
SAFEOP or OP stays there.

    python restore-default-parameters.py
    python restore-default-parameters.py --group application --slave 2
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

PROCEDURE = "restore-default-parameters"


def main():
    ap = parser(__doc__)
    ap.add_argument(
        "--slave", type=integer, default=1, help="the position of the device on the bus"
    )
    ap.add_argument(
        "--group",
        choices=["all", "communication", "application", "manufacturer"],
        default="all",
        help="the 0x1011 group to restore",
    )
    args = ap.parse_args()
    body = {"group": args.group}

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(AlState.PREOP, positions=[args.slave])

    run_to_end(client, args.slave, PROCEDURE, body)


if __name__ == "__main__":
    run(main)
