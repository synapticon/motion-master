#!/usr/bin/env python3
"""Print the CiA 402 state of a drive with its actual and target values (GET .../cia402).

CiA 402 is the device profile for drives. The server decodes the Statusword into the state of the
profile's state machine, and reads the mode of operation, the actual values, the targets and the
drive's error report.

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. A device in
SAFE-OP or OP stays there.

    python cia402-state.py
    python cia402-state.py --slave 2
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import PRE_OP, MotionMasterClient, run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(PRE_OP, positions=[args.slave])

    s = client.call("getCia402Status", path={"slavePosition": args.slave}).json()
    print(
        f"state {s['state']}, statusword 0x{s['statusword']:04x}, "
        f"controlword 0x{s['controlword']:04x}"
    )
    print(f"mode {s['modeName']} ({s['modeOfOperation']}), error report {s['errorDescription']}")
    print(f"  position  actual {s['actualPosition']:>12}  target {s['targetPosition']:>12}")
    print(f"  velocity  actual {s['actualVelocity']:>12}  target {s['targetVelocity']:>12}")
    print(f"  torque    actual {s['actualTorque']:>12}  target {s['targetTorque']:>12}")


if __name__ == "__main__":
    run(main)
