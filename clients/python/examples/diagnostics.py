#!/usr/bin/env python3
"""Print the ESC error counters and the distributed clock sync of the devices.

Reads GET /api/devices/diagnostics and GET /api/dc-sync. The counters come live from the ESC
(EtherCAT Slave Controller) registers of each device. Each counter is 8 bits wide and stops at
255. Only a power cycle or a write to the register clears it, so a counter that is not zero says
that errors happened, not when.

Opens the fieldbus driver and scans first when needed, and moves no device.

    python diagnostics.py
    python diagnostics.py --slave 2
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import MotionMasterClient  # noqa: E402

PORT_COUNTERS = ("rxError", "invalidFrame", "forwardedError", "lostLink")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, help="one device position (default: every device)")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_scanned()

    query = {"positions": str(args.slave)} if args.slave else None
    for d in client.call("getDeviceDiagnostics", query=query).json():
        print(
            f"device {d['slavePosition']} {d['deviceName']}: processing unit errors "
            f"{d['processingUnitError']}, PDI errors {d['pdiError']}, PDI watchdog "
            f"{d['pdiWatchdog']}, process data watchdog {d['processDataWatchdog']}"
        )
        for n, port in enumerate(d["ports"]):
            if not port["linkUp"]:
                continue
            counters = ", ".join(f"{c} {port[c]}" for c in PORT_COUNTERS)
            print(f"  port {n}: link up, {counters}")

    print("distributed clock:")
    for d in client.call("getDcSync", query=query).json():
        if not d["dcCapable"]:
            print(f"  device {d['slavePosition']}: not DC-capable")
            continue
        role = "reference clock" if d["referenceClock"] else "disciplined"
        print(
            f"  device {d['slavePosition']}: {role}, system time difference "
            f"{d['systemTimeDifference']} ns, propagation delay {d['propagationDelay']} ns"
        )


if __name__ == "__main__":
    main()
