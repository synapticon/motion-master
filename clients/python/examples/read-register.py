#!/usr/bin/env python3
"""Read bytes from an ESC register of a device (GET .../registers/{address}?length=N).

The script prints the bytes and, for a read of up to 8 bytes, their little-endian unsigned value.
The ESC (EtherCAT Slave Controller) answers in every AL state.

Opens the fieldbus driver and scans first when needed, and moves no device.

The defaults read the type, revision and build of the ESC, 4 bytes at 0x0000.

    python read-register.py
    python read-register.py --address 0x0130 --length 2 --slave 2
"""

import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import MotionMasterClient, run  # noqa: E402
from options import integer, parser  # noqa: E402


def main():
    ap = parser(__doc__)
    ap.add_argument(
        "--slave", type=integer, default=1, help="the position of the device on the bus"
    )
    ap.add_argument(
        "--address", type=integer, default="0x0000", help="the address of the register"
    )
    ap.add_argument("--length", type=integer, default=4, help="number of bytes")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_scanned()

    data = client.call(
        "readRegister",
        path={"slavePosition": args.slave, "address": args.address},
        query={"length": args.length},
    ).json()["data"]

    line = f"slave {args.slave} 0x{args.address:04X}: {bytes(data).hex(' ').upper()}"
    if len(data) <= 8:
        value = int.from_bytes(bytes(data), "little")
        line += f" = {value} (0x{value:x})"
    print(line)


if __name__ == "__main__":
    run(main)
