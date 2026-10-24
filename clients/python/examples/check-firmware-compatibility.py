#!/usr/bin/env python3
"""Check whether a firmware package belongs on a device (GET .../firmware-compatibility).

The server reads the .hardware_description and .variant files of the device, and compares the
firmware descriptors the device accepts with the descriptor in the package filename. The script
needs only the filename, not the package. It installs nothing, and the firmware installation
procedure does not check the answer. The script exits with 1 when the package does not belong
on the device.

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. A device in
SAFE-OP or OP stays there. A device in BOOT goes through INIT to PRE-OP.

    python check-firmware-compatibility.py --filename package_ACTILINK_6000-05-2423-1_motion-drive_v5.6.11.zip
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
    ap.add_argument("--filename", required=True, help="the package filename, a path is allowed")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(PRE_OP, positions=[args.slave])

    r = client.call(
        "checkDeviceFirmwareCompatibility",
        path={"slavePosition": args.slave},
        query={"filename": Path(args.filename).name},
        check=False,
    )
    # A 4xx means that the server could not ask the question, for example because the filename is
    # not a package name. Its error says why.
    if r.status_code != 200:
        sys.exit(r.json().get("error", r.text))

    result = r.json()
    print(f"{'compatible' if result['compatible'] else 'not compatible'}: {result['explanation']}")
    if not result["compatible"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
