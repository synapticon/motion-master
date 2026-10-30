#!/usr/bin/env python3
"""Commission an Integro drive, step by step, and keep a record of what was done.

Run it in the directory that holds the firmware package and the configuration file. The log and
the record go to logs/ beside this script.

    python commission.py
    python commission.py --slave 2
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common import Session, read_hardware_description  # noqa: E402
from motion_master_client import run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    args = ap.parse_args()

    with Session(args.slave, Path(__file__).resolve().parent / "logs") as s:
        s.connect()
        read_hardware_description(s)


if __name__ == "__main__":
    run(main)
