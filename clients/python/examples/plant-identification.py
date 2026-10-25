#!/usr/bin/env python3
"""Identify the plant model of a drive, and write plant_model.csv (plant-identification).

The plant model is the transfer function from torque to velocity of the motor and its load, from
which auto-tuning computes controller gains. The procedure enables the drive in cyclic synchronous
torque mode with a zero target, runs a chirp, records the velocity and the torque, and fits the
model with the auto-tuning program of the server. The chirp turns the motor in both directions
for its whole duration, so the shaft must be free and its load safe to move. The drive records at
most 6000 ms, so --time can be at most 5800 ms. The amplitude is a torque command, and neither the
server nor the drive checks it. The controller gains do not change. Ctrl-C cancels the run.

Opens the fieldbus driver, scans and moves the device to OP first when needed, because the drive
enables only while it exchanges process data.

    python plant-identification.py --start-frequency 1000 --target-frequency 100000 \
        --amplitude 50 --time 5000
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import (  # noqa: E402
    OP,
    MotionMasterClient,
    ProcedureError,
    format_step,
    run,
    step_changes,
)

PROCEDURE = "plant-identification"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--start-frequency", type=int, required=True, help="sweep start, mHz")
    ap.add_argument("--target-frequency", type=int, required=True, help="sweep end, mHz")
    ap.add_argument(
        "--amplitude", type=int, required=True, help="peak torque, per mille of rated torque"
    )
    ap.add_argument("--time", type=int, required=True, help="sweep duration, ms")
    ap.add_argument("--signal", choices=["logarithmic", "linear"], default="logarithmic")
    args = ap.parse_args()
    body = {
        "startFrequencyMilliHz": args.start_frequency,
        "targetFrequencyMilliHz": args.target_frequency,
        "targetAmplitudePermil": args.amplitude,
        "transitionTimeMs": args.time,
        "signalType": args.signal,
    }

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(OP, positions=[args.slave])

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
