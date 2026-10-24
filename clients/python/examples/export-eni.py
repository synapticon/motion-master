#!/usr/bin/env python3
"""Save the bus as an ENI file (GET /api/eni).

An ENI (EtherCAT Network Information, ETG.2100) is the configuration that another EtherCAT master
replays to bring the same bus up. The server builds it from the live bus. The file goes into the
git-ignored files/ directory next to the client. Pass --output for a different path.

Opens the fieldbus driver, scans and moves the devices to SAFE-OP first when needed, because the
FMMUs and the logical addresses of the bus exist only from the SAFE-OP transition on. A device in
OP stays there.

    python export-eni.py
    python export-eni.py --output /tmp/bus.xml
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import SAFE_OP, MotionMasterClient  # noqa: E402

FILES_DIR = Path(__file__).resolve().parent.parent / "files"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", help="local path to save to (default: files/eni.xml)")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(SAFE_OP)

    content = client.call("exportEni").content
    out = Path(args.output) if args.output else FILES_DIR / "eni.xml"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(content)
    print(f"saved {len(content)} byte(s) -> {out}")


if __name__ == "__main__":
    main()
