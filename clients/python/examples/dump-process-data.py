#!/usr/bin/env python3
"""Save the recorder of the server as a .mmpd file (GET /api/process-data/dump).

The server records the process data of every cycle in a ring. The dump holds every cycle that the
ring still has, with the layout of the process image in its header, so that a reader can decode
it offline. The Console and the TypeScript client read .mmpd files. The file goes into the
git-ignored files/ directory next to the client. Pass --output for a different path.

Opens the fieldbus driver, scans and moves the device to SAFE-OP first when needed, because the
recorder holds cycles only once the process data is exchanged. A device in OP stays there.

    python dump-process-data.py
    python dump-process-data.py --output /tmp/bus.mmpd
"""

import argparse
import logging
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import SAFE_OP, MotionMasterClient, run  # noqa: E402

FILES_DIR = Path(__file__).resolve().parent.parent / "files"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--output", help="local path to save to (default: files/process-data-<time>.mmpd)"
    )
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(SAFE_OP)

    content = client.call(
        "streamProcessDataDump", headers={"Accept": "application/octet-stream"}
    ).content

    if args.output:
        out = Path(args.output)
    else:
        out = FILES_DIR / f"process-data-{datetime.now():%Y%m%d-%H%M%S}.mmpd"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(content)
    print(f"saved {len(content)} byte(s) -> {out}")


if __name__ == "__main__":
    run(main)
