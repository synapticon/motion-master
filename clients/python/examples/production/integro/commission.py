#!/usr/bin/env python3
"""Commission an Integro drive, step by step, and keep a record of what was done.

Firmware packages come from production/packages/. The log and the record go to logs/ beside
this script. See production/README.md for the steps.

    python commission.py
    python commission.py --slave 2
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common import Session, connect, install_firmware, read_hardware_description  # noqa: E402
from motion_master_client import run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    args = ap.parse_args()

    with Session(args.slave, Path(__file__).resolve().parent / "logs") as s:
        connect(s)
        read_hardware_description(s)
        install_firmware(s)


if __name__ == "__main__":
    run(main)
