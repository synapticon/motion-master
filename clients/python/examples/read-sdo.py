#!/usr/bin/env python3
"""Read one object dictionary entry with a CoE SDO upload (GET .../sdo/{index}/{subindex}).

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed, because the
mailbox is live from PRE-OP up. A device in SAFE-OP or OP stays there. The server returns the
raw bytes of the object. The script prints them and their little-endian unsigned value.

The defaults read the Statusword, 0x6041:00.

    python read-sdo.py
    python read-sdo.py --slave 2 --index 0x6064 --subindex 0
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import PRE_OP, MotionMasterClient, run  # noqa: E402


def auto_int(text):
    """Parse a decimal or 0x-prefixed hex integer."""
    return int(text, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--index", type=auto_int, default=0x6041)
    ap.add_argument("--subindex", type=auto_int, default=0)
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(PRE_OP, positions=[args.slave])

    data = client.call(
        "sdoUpload",
        path={"slavePosition": args.slave, "index": args.index, "subindex": args.subindex},
    ).json()["data"]

    value = int.from_bytes(bytes(data), "little")
    print(
        f"slave {args.slave} 0x{args.index:04x}:{args.subindex:02x} = "
        f"{value} (0x{value:x}), {len(data)} byte(s): {data}"
    )


if __name__ == "__main__":
    run(main)
