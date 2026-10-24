#!/usr/bin/env python3
"""List the files stored on a device with their sizes (GET /api/devices/{slavePosition}/files).

The size is what the file list of the device reports. A file name the device reports with no size
is shown without one.

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. A device in
SAFE-OP or OP stays there. A device in BOOT goes through INIT to PRE-OP.

    python list-files.py
    python list-files.py --slave 2
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import PRE_OP, MotionMasterClient  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(PRE_OP, positions=[args.slave])

    files = client.call("listDeviceFiles", path={"slavePosition": args.slave}).json()
    for f in files:
        size = f"{f['byteCount']:>8} bytes" if "byteCount" in f else " " * 14
        print(f"  {size}  {f['name']}")
    print(f"{len(files)} file(s)")


if __name__ == "__main__":
    main()
