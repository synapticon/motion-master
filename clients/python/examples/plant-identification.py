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

PROCEDURE = "plant-identification"


def main():
    ap = parser(__doc__)
    ap.add_argument(
        "--slave", type=integer, default=1, help="the position of the device on the bus"
    )
    ap.add_argument("--start-frequency", type=integer, required=True, help="sweep start, mHz")
    ap.add_argument("--target-frequency", type=integer, required=True, help="sweep end, mHz")
    ap.add_argument(
        "--amplitude", type=integer, required=True, help="peak torque, per mille of rated torque"
    )
    ap.add_argument("--time", type=integer, required=True, help="sweep duration, ms")
    ap.add_argument(
        "--signal",
        choices=["logarithmic", "linear"],
        default="logarithmic",
        help="the shape of the sweep",
    )
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
    client.ensure_state(AlState.OP, positions=[args.slave])

    run_to_end(client, args.slave, PROCEDURE, body)


if __name__ == "__main__":
    run(main)
