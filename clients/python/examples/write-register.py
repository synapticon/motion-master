#!/usr/bin/env python3
"""Write bytes to an ESC register of a device (POST .../registers/{address}).

--data gives the bytes in register order, separated by commas. The server writes them with one
FPWR datagram to the ESC (EtherCAT Slave Controller). It does not check what the register does.
read-register.py reads the register back.

Opens the fieldbus driver and scans first when needed, and moves no device.

The example clears the invalid frame and RX error counters of ports 0 to 3, 8 bytes at 0x0300,
which only a write or a power cycle clears.

    python write-register.py --address 0x0300 --data 0,0,0,0,0,0,0,0
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import MotionMasterClient, run  # noqa: E402


def auto_int(text):
    """Parse a decimal or 0x-prefixed hex integer."""
    return int(text, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--address", type=auto_int, required=True)
    ap.add_argument("--data", required=True, help="bytes separated by commas, such as 0,0")
    args = ap.parse_args()
    data = [int(b, 0) for b in args.data.split(",")]

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_scanned()

    r = client.call(
        "writeRegister",
        path={"slavePosition": args.slave, "address": args.address},
        body={"data": data},
        check=False,
    )
    if r.status_code != 200:
        sys.exit(r.json().get("error", r.text))
    print(f"slave {args.slave} 0x{args.address:04x} <- {bytes(data).hex(' ')}")


if __name__ == "__main__":
    run(main)
