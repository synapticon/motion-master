#!/usr/bin/env python3
"""Scan the bus and list the devices it finds (POST /api/scan).

Opens the fieldbus driver from config.toml first when none is open. A scan
leaves every device in INIT. An empty bus is a successful scan that finds no
devices, not an error.

    python scan.py
"""

import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import AlState, MotionMasterClient, run  # noqa: E402


def main():
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()

    found = client.scan()
    if found == 0:
        return

    states = {d["slavePosition"]: d for d in client.call("getDeviceStates").json()}
    for d in client.call("getDevices").json():
        state = AlState(states[d["slavePosition"]]["alState"]).name
        print(
            f"  {d['slavePosition']:3d}  {d['productName']:<30}  serial {d['serialNumber']:<12}"
            f"  revision 0x{d['revisionNumber']:08x}  {state}"
        )


if __name__ == "__main__":
    run(main)
