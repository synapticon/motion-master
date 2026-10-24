#!/usr/bin/env python3
"""Write a local file to a device over FoE (PUT /api/devices/{slavePosition}/files/{filename}).

The file keeps its local name on the device unless --filename gives another one. A file of that
name on the device is replaced. On a SOMANET drive, config.csv holds the stored parameter
values, so a write to it changes the drive after the next power cycle. An Integro accepts a write
only to a file name on its whitelist. It answers another name with FoE error 0x8001, "Path not
found".

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. A device in
SAFE-OP or OP stays there. A device in BOOT goes through INIT to PRE-OP.

    python write-file.py --file notes.txt
    python write-file.py --file files/config.csv --filename config.csv --slave 2
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
    ap.add_argument("--file", required=True, help="the local file to write")
    ap.add_argument("--filename", help="the name on the device (default: the local name)")
    args = ap.parse_args()

    local = Path(args.file)
    filename = args.filename or local.name
    content = local.read_bytes()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(PRE_OP, positions=[args.slave])

    r = client.call(
        "foeWriteFile",
        path={"slavePosition": args.slave, "filename": filename},
        data=content,
        headers={"Content-Type": "application/octet-stream"},
        check=False,
    )
    if r.status_code != 200:
        sys.exit(r.json().get("error", r.text))
    print(f"wrote {len(content)} byte(s) from {local} -> slave {args.slave}:{filename}")


if __name__ == "__main__":
    main()
