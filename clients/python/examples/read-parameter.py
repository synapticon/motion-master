#!/usr/bin/env python3
"""Read one parameter with its name, data type and value (GET .../parameters/{index}/{subindex}).

The server reads the value from the process image when the object is PDO-mapped and the device
exchanges process data, and with an SDO read otherwise. --cache returns the value the server holds
and touches no bus. read-sdo.py reads the raw bytes of an object instead, with no decoding.

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. The server
learns the parameters of a device when it reaches PRE-OP. A device in SAFE-OP or OP stays there.

The defaults read the Statusword, 0x6041:00.

    python read-parameter.py
    python read-parameter.py --index 0x6064 --subindex 0 --slave 2
    python read-parameter.py --index 0x2031 --subindex 1 --cache
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import PRE_OP, MotionMasterClient  # noqa: E402


def auto_int(text):
    """Parse a decimal or 0x-prefixed hex integer."""
    return int(text, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--index", type=auto_int, default=0x6041)
    ap.add_argument("--subindex", type=auto_int, default=0)
    ap.add_argument("--cache", action="store_true", help="return the cached value, read no bus")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(PRE_OP, positions=[args.slave])

    p = client.call(
        "readParameter",
        path={"slavePosition": args.slave, "index": args.index, "subindex": args.subindex},
        query={"source": "cache"} if args.cache else None,
    ).json()
    print(
        f"slave {args.slave} 0x{p['index']:04x}:{p['subindex']:02x} {p['name']} "
        f"({p['dataTypeName']}) = {p['value']!r}"
    )


if __name__ == "__main__":
    main()
