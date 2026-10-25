#!/usr/bin/env python3
"""Read a file from a device over FoE (GET /api/devices/{slavePosition}/files/{filename}).

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. A device in
SAFE-OP or OP stays there. A device in BOOT goes through INIT to PRE-OP. The server returns the
raw bytes of the file, and the script writes them to disk.

The file goes into the git-ignored files/ directory next to the client, so a download never
shows up as untracked in the repository. Pass --output for a different path.

    python read-file.py --filename .hardware_description
    python read-file.py --slave 2 --filename config.csv --output /tmp/config.csv
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import PRE_OP, MotionMasterClient, run  # noqa: E402

FILES_DIR = Path(__file__).resolve().parent.parent / "files"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--filename", required=True, help="FoE filename as the firmware knows it")
    ap.add_argument("--output", help="local path to save to (default: files/<filename>)")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(PRE_OP, positions=[args.slave])

    content = client.call(
        "foeReadFile", path={"slavePosition": args.slave, "filename": args.filename}
    ).content

    out = Path(args.output) if args.output else FILES_DIR / Path(args.filename).name
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(content)
    print(f"read {len(content)} byte(s) from slave {args.slave}:{args.filename} -> {out}")


if __name__ == "__main__":
    run(main)
