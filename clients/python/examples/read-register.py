#!/usr/bin/env python3
"""Read bytes from an ESC register of a device (GET .../registers/{address}?length=N).

The script prints the bytes and, for a read of up to 8 bytes, their little-endian unsigned value.
The ESC (EtherCAT Slave Controller) answers in every AL state.

Opens the fieldbus driver and scans first when needed, and moves no device.

The defaults read the type, revision and build of the ESC, 4 bytes at 0x0000.

    python read-register.py
    python read-register.py --address 0x0130 --length 2 --slave 2
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import MotionMasterClient  # noqa: E402


def auto_int(text):
    """Parse a decimal or 0x-prefixed hex integer."""
    return int(text, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--address", type=auto_int, default=0x0000)
    ap.add_argument("--length", type=auto_int, default=4, help="number of bytes")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_scanned()

    data = client.call(
        "readRegister",
        path={"slavePosition": args.slave, "address": args.address},
        query={"length": args.length},
    ).json()["data"]

    line = f"slave {args.slave} 0x{args.address:04x}: {bytes(data).hex(' ')}"
    if len(data) <= 8:
        value = int.from_bytes(bytes(data), "little")
        line += f" = {value} (0x{value:x})"
    print(line)


if __name__ == "__main__":
    main()
