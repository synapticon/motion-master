#!/usr/bin/env python3
"""Delete a file from the flash of a SOMANET device (DELETE .../files/{filename}).

The file is gone from flash at once. A file that was not there also succeeds, because the server
answers that the file is gone. The device answers only after it erased the file, which can take
seconds. Deleting config.csv discards the stored parameter values, so the drive starts with its
defaults after the next power cycle. A device that is not a SOMANET device is refused.

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. A device in
SAFE-OP or OP stays there. A device in BOOT goes through INIT to PRE-OP.

    python delete-file.py --filename notes.txt
    python delete-file.py --filename hr_data0.bin --slave 2
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import PRE_OP, MotionMasterClient, run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--filename", required=True, help="the name of the file on the device")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(PRE_OP, positions=[args.slave])

    r = client.call(
        "deleteDeviceFile",
        path={"slavePosition": args.slave, "filename": args.filename},
        check=False,
    )
    if r.status_code != 200:
        sys.exit(r.json().get("error", r.text))
    print(f"slave {args.slave}:{args.filename} is gone")


if __name__ == "__main__":
    run(main)
