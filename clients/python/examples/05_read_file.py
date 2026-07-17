#!/usr/bin/env python3
"""Read a file from a device via FoE (GET /api/devices/{slavePosition}/files/{filename}).

The response is the raw file bytes (application/octet-stream), written to disk.
FoE is device-dependent about which AL states it allows; PRE-OP or above is a
safe bet, so run 03_transition_preop first.

    python 05_read_file.py --slave 1 --filename log.csv --output log.csv
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common import make_client  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--filename", required=True, help="FoE filename as recognised by the firmware")
    ap.add_argument("--output", help="local path to save to (default: the FoE filename)")
    args = ap.parse_args()

    client = make_client()
    content = client.call(
        "foeReadFile", path={"slavePosition": args.slave, "filename": args.filename}
    ).content

    out = Path(args.output or args.filename)
    out.write_bytes(content)
    print(f"read {len(content)} byte(s) from slave {args.slave}:{args.filename} -> {out}")


if __name__ == "__main__":
    main()
