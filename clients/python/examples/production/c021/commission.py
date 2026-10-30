#!/usr/bin/env python3
"""Commission a C021 drive, step by step, and keep a record of what was done.

The C021 is a Jasper-based board. The script runs the steps up to the
assigned name. The encoder, motion and tuning steps follow when a C021 is at hand to test them.

Firmware packages come from production/packages/. The configuration comes from config.csv beside
this script. The log and the record go to logs/ beside this script. See production/README.md for
the steps.

    python commission.py
    python commission.py --slave 2
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common import (  # noqa: E402
    Session,
    connect,
    install_firmware,
    load_configuration,
    read_hardware_description,
    set_assigned_name,
    transition_to_state,
)
from motion_master_client import PRE_OP, run  # noqa: E402

LOGS_DIR = Path(__file__).resolve().parent / "logs"
CONFIG_FILE = Path(__file__).resolve().parent / "config.csv"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    args = ap.parse_args()

    with Session(args.slave, LOGS_DIR) as s:
        connect(s)
        transition_to_state(s, PRE_OP)
        read_hardware_description(s)
        install_firmware(s)
        load_configuration(s, CONFIG_FILE)
        set_assigned_name(s)


if __name__ == "__main__":
    run(main)
