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

Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
SAFEOP or OP stays there.

    python system-identification.py --start-frequency 1000 --target-frequency 100000 \
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

PROCEDURE = "system-identification"


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
    ap.add_argument(
        "--start",
        choices=["none", "immediately", "after-hrd-stream-start"],
        default="none",
        help="when the chirp starts",
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
    client.ensure_state(AlState.PREOP, positions=[args.slave])

    run_to_end(client, args.slave, PROCEDURE, body)


if __name__ == "__main__":
    run(main)
