#!/usr/bin/env python3
"""Write raw bytes to one object with an SDO download (PUT .../sdo/{index}/{subindex}).

--data gives the bytes in wire order, little-endian, separated by commas. The byte count must
match the length of the object. write-parameter.py converts a value to the data type instead,
which is the easier way for most objects.

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed, because the
mailbox is live from PRE-OP up. A device in SAFE-OP or OP stays there.

    python write-sdo.py --index 0x6060 --subindex 0 --data 8
    python write-sdo.py --index 0x607a --subindex 0 --data 0x10,0x27,0,0 --slave 2
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
    ap.add_argument("--index", type=auto_int, required=True)
    ap.add_argument("--subindex", type=auto_int, required=True)
    ap.add_argument("--data", required=True, help="bytes separated by commas, such as 0x10,0x27")
    args = ap.parse_args()
    data = [int(b, 0) for b in args.data.split(",")]

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(PRE_OP, positions=[args.slave])

    path = {"slavePosition": args.slave, "index": args.index, "subindex": args.subindex}
    r = client.call("sdoDownload", path=path, body={"data": data}, check=False)
    if r.status_code != 200:
        sys.exit(r.json().get("error", r.text))
    print(f"slave {args.slave} 0x{args.index:04x}:{args.subindex:02x} <- {bytes(data).hex(' ')}")


if __name__ == "__main__":
    run(main)
