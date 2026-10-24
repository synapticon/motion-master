#!/usr/bin/env python3
"""Restore the default parameter values of a device (procedure restore-default-parameters).

The procedure writes the "load" signature to 0x1011 for the group you choose, and polls the
device until it reports the restore complete. The live values of that group are replaced, so
anything not stored is gone. The defaults go into volatile memory: run store-parameters.py to keep
them, and some devices apply them only after a reset or a power cycle. Ctrl-C stops the wait, not
the restore, because the device already has the command.

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. A device in
SAFE-OP or OP stays there.

    python restore-default-parameters.py
    python restore-default-parameters.py --group application --slave 2
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

PROCEDURE = "restore-default-parameters"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument(
        "--group",
        choices=["all", "communication", "application", "manufacturer"],
        default="all",
        help="the 0x1011 group to restore (default: all)",
    )
    args = ap.parse_args()
    body = {"group": args.group}

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
