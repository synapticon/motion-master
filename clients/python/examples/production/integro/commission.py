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
from common import (  # noqa: E402
    Session,
    connect,
    detect_offset,
    install_firmware,
    load_configuration,
    read_hardware_description,
    set_assigned_name,
    tune_controller,
    transition_to_state,
    identify_plant,
)
from motion_master_client import OP, PRE_OP, run  # noqa: E402

# The rotating method, which needs no tuning and holds the load.
OFFSET_DETECTION_METHOD = 0

# The chirp of the last plant identification stored on the bench Integro.
PLANT_IDENTIFICATION_CHIRP = {
    "startFrequencyMilliHz": 2300,
    "targetFrequencyMilliHz": 31000,
    "targetAmplitudePermil": 460,
    "transitionTimeMs": 3000,
    "signalType": "logarithmic",
}

# Full tuning lets the auto-tuning program choose the targets. PI-P puts the integral action in
# the position loop.
VELOCITY_TUNING = {"full": True}
POSITION_TUNING = {"full": True, "controllerType": "PI-P"}

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
        transition_to_state(s, OP)
        detect_offset(s, OFFSET_DETECTION_METHOD)
        identify_plant(s, PLANT_IDENTIFICATION_CHIRP)
        tune_controller(s, "velocity", VELOCITY_TUNING)
        tune_controller(s, "position", POSITION_TUNING)


if __name__ == "__main__":
    run(main)
