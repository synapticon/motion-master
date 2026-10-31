#!/usr/bin/env python3
"""Commission a C021 drive, step by step, and keep a record of what was done.

The C021 is a Jasper-based board. Firmware packages come from production/packages/. The
configuration comes from config.csv beside this script. The log and the record go to logs/ beside
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
    calibrate_encoder,
    configure_encoder,
    connect,
    detect_offset,
    identify_plant,
    install_firmware,
    load_configuration,
    read_hardware_description,
    set_assigned_name,
    set_integral_limits,
    transition_to_state,
    tune_controller,
)
from motion_master_client import OP, PRE_OP, run  # noqa: E402

# Jasper firmware has no 0x2009:03, so the drive picks the method of the offset detection itself.
OFFSET_DETECTION_METHOD = None

# The chirp of the Integro script, not yet measured on a C021.
PLANT_IDENTIFICATION_CHIRP = {
    "startFrequencyMilliHz": 2300,
    "targetFrequencyMilliHz": 31000,
    "targetAmplitudePermil": 460,
    "transitionTimeMs": 3000,
    "signalType": "logarithmic",
}

# Full tuning lets the auto-tuning program choose the targets. Control strategy 2 is the cascaded
# position controller, and PI-P puts the integral action in its position loop.
VELOCITY_TUNING = {"full": True}
POSITION_TUNING = {"controlStrategy": 2, "full": True, "controllerType": "PI-P"}

# The C021 has an iC-MU encoder on each of sensor ports 1 and 2. Encoder 2 (0x2112) is on the
# motor shaft, and commutation and velocity control use it. Encoder 1 (0x2110) is the one position
# control uses. Jasper firmware takes only the singleturn path, so the configuration takes no
# battery mode.
ENCODER_ORDINALS = (2, 1)

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
        set_integral_limits(s)
        for ordinal in ENCODER_ORDINALS:
            configure_encoder(s, {"encoderOrdinal": ordinal, "writeRestoreFile": True})
        commutation = False
        for ordinal in ENCODER_ORDINALS:
            commutation |= calibrate_encoder(s, {"encoderOrdinal": ordinal})
        if commutation:
            detect_offset(s, OFFSET_DETECTION_METHOD)


if __name__ == "__main__":
    run(main)
