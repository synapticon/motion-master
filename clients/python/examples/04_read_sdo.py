#!/usr/bin/env python3
"""Read an object dictionary entry via a raw CoE SDO upload.

GET /api/devices/{slavePosition}/sdo/{index}/{subindex} returns the raw object
bytes. Requires the device to be in PRE-OP or above (mailbox active), so run
03_transition_preop first.

Defaults read the CiA402 Statusword (0x6041:0). Override on the command line:
    python 04_read_sdo.py --slave 1 --index 0x6064 --subindex 0
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common import make_client  # noqa: E402


def auto_int(text):
    """Parse a decimal or 0x-prefixed hex integer."""
    return int(text, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--index", type=auto_int, default=0x6041)
    ap.add_argument("--subindex", type=auto_int, default=0)
    args = ap.parse_args()

    client = make_client()
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
    main()
