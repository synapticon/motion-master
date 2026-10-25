#!/usr/bin/env python3
"""Configure the system identification chirp of a drive, and arm it (system-identification).

The chirp is a torque sweep from a start frequency to a target frequency over a time you give:
logarithmic, with an amplitude that rises from half the target, or linear, at a constant
amplitude. With --start none, the default, the procedure configures the drive and excites
nothing. With --start immediately, the chirp starts on the next control cycle if the drive is
enabled, so the shaft must be free. With --start after-hrd-stream-start, it waits for a high
resolution data recording to begin, which is how the response gets recorded.

The amplitude is a torque command, and neither the server nor the drive checks it. The server
checks the frequency and time bounds before it writes anything. Ctrl-C cancels the run.

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. A device in
SAFE-OP or OP stays there.

    python system-identification.py --start-frequency 1000 --target-frequency 100000 \
        --amplitude 50 --time 5000
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

PROCEDURE = "system-identification"


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
    ap.add_argument(
        "--start", choices=["none", "immediately", "after-hrd-stream-start"], default="none"
    )
    args = ap.parse_args()
    body = {
        "startFrequencyMilliHz": args.start_frequency,
        "targetFrequencyMilliHz": args.target_frequency,
        "targetAmplitudePermil": args.amplitude,
        "transitionTimeMs": args.time,
        "signalType": args.signal,
        "start": args.start,
    }

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
