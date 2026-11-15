#!/usr/bin/env python3
"""Store the current parameter values of a device in its flash (procedure store-parameters).

The procedure writes the "save" signature to 0x1010 and polls the device until it reports the
store complete, which takes about a second. It stores every value the device holds, so it also
keeps every change written since the last store. Without a store, a power cycle loses them.
Ctrl-C stops the wait, not the store, because the device already has the command.

Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
SAFEOP or OP stays there.

    python store-parameters.py
    python store-parameters.py --slave 2
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

PROCEDURE = "store-parameters"


def main():
    ap = parser(__doc__)
    ap.add_argument(
        "--slave", type=integer, default=1, help="the position of the device on the bus"
    )
    args = ap.parse_args()
    body = {}

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(AlState.PREOP, positions=[args.slave])

    run_to_end(client, args.slave, PROCEDURE)


if __name__ == "__main__":
    run(main)
