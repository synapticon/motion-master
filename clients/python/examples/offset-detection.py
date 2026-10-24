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

import argparse
import json
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import OP, MotionMasterClient  # noqa: E402

PROCEDURE = "offset-detection"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(OP, positions=[args.slave])

    # A step that starts and ends between two polls shows only once, already finished, so the
    # script prints each change of status rather than each poll. A step not reached yet is idle,
    # and stays unprinted until it starts.
    printed = {}
    snapshot = None
    run = client.run_procedure(args.slave, PROCEDURE)
    try:
        for snapshot in run:
            for step in snapshot["steps"]:
                if printed.get(step["id"], "idle") == step["status"]:
                    continue
                printed[step["id"]] = step["status"]
                line = f"  {step['id']:<36} {step['status']}"
                if "value" in step:
                    line += f"  {json.dumps(step['value'], ensure_ascii=False)}"
                if "error" in step:
                    line += f"  {step['error']}"
                print(line)
    except KeyboardInterrupt:
        print("\ncancelled")
        sys.exit(1)
    finally:
        # Closing the generator cancels a run that is still in progress.
        run.close()

    if snapshot.get("error"):
        print(f"error: {snapshot['error']}")
    print(f"{PROCEDURE} {snapshot['status']}")
    if snapshot["status"] != "succeeded":
        sys.exit(1)


if __name__ == "__main__":
    main()
