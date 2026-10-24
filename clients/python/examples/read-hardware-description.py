#!/usr/bin/env python3
"""Print what a device says it is (GET /api/devices/{slavePosition}/hardware-description).

The server reads the .hardware_description file of the device over FoE and decodes it. The file
names the device and, when the device is part of one, the assembly, with the build descriptor,
the serial number and the components of each. A device that is not a SOMANET device has no such
file, and the request fails.

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. A device in
SAFE-OP or OP stays there. A device in BOOT goes through INIT to PRE-OP.

    python read-hardware-description.py
    python read-hardware-description.py --slave 2
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import PRE_OP, MotionMasterClient  # noqa: E402


def print_part(title, part):
    """Print one part of a hardware description: the device or the assembly."""
    print(f"{title}: {part['name']}")
    print(f"  build descriptor {part['buildDescriptor']}, serial number {part['serialNumber']}")
    for c in part.get("components", []):
        version = f", version {c['version']}" if c.get("version") else ""
        print(f"  - {c['name']}, serial number {c['serialNumber']}{version}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(PRE_OP, positions=[args.slave])

    hd = client.call("getDeviceHardwareDescription", path={"slavePosition": args.slave}).json()
    print_part("device", hd["device"])
    if hd.get("assembly"):
        print_part("assembly", hd["assembly"])


if __name__ == "__main__":
    main()
