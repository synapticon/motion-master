#!/usr/bin/env python3
"""Blink the LEDs of a device, so that someone at the machine can find it (device-locate).

The LEDs blink for --duration seconds and stop at the end of the run, or at once on Ctrl-C. Only
a fieldbus driver that can blink the LEDs offers the procedure, which today is SPoE. Over SOEM the
device has no device-locate procedure, and the server refuses the run with 404.

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. A device in
SAFE-OP or OP stays there.

    python device-locate.py
    python device-locate.py --duration 30 --slave 2
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
    run,
    step_changes,
)

PROCEDURE = "device-locate"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--duration", type=int, default=10, help="seconds, 1 to 3600 (default: 10)")
    args = ap.parse_args()
    body = {"duration": args.duration}

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
    run(main)
