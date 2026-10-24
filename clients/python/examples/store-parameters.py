#!/usr/bin/env python3
"""Store the current parameter values of a device in its flash (procedure store-parameters).

The procedure writes the "save" signature to 0x1010 and polls the device until it reports the
store complete, which takes about a second. It stores every value the device holds, so it also
keeps every change written since the last store. Without a store, a power cycle loses them.
Ctrl-C stops the wait, not the store, because the device already has the command.

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. A device in
SAFE-OP or OP stays there.

    python store-parameters.py
    python store-parameters.py --slave 2
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import (  # noqa: E402
    PRE_OP,
    MotionMasterClient,
    ProcedureError,
    format_step,
    step_changes,
)

PROCEDURE = "store-parameters"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    args = ap.parse_args()
    body = {}

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(PRE_OP, positions=[args.slave])

    snapshot = None
    changes = step_changes(client.run_procedure(args.slave, PROCEDURE, body))
    try:
        for snapshot, changed in changes:
            for step in changed:
                print(format_step(step))
    except ProcedureError as e:
        sys.exit(str(e))
    except KeyboardInterrupt:
        print("\ncancelled")
        sys.exit(1)
    finally:
        # Closing the generator cancels a run that is still in progress.
        changes.close()

    if snapshot.get("error"):
        print(f"error: {snapshot['error']}")
    print(f"{PROCEDURE} {snapshot['status']}")
    if snapshot["status"] != "succeeded":
        sys.exit(1)


if __name__ == "__main__":
    main()
