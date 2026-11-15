#!/usr/bin/env python3
"""Read one object dictionary entry with a CoE SDO upload (GET .../sdo/{index}/{subindex}).

Opens the fieldbus driver, scans and moves the device to PREOP first when needed, because the
mailbox is live from PREOP up. A device in SAFEOP or OP stays there. The server returns the
raw bytes of the object. The script prints them and their little-endian unsigned value.

The defaults read the Statusword, 0x6041:00.

    python read-sdo.py
    python read-sdo.py --slave 2 --index 0x6064 --subindex 0
"""

import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import AlState, MotionMasterClient, object_address_text, run  # noqa: E402
from options import integer, parser  # noqa: E402


def main():
    ap = parser(__doc__)
    ap.add_argument(
        "--slave", type=integer, default=1, help="the position of the device on the bus"
    )
    ap.add_argument("--index", type=integer, default="0x6041", help="the index of the object")
    ap.add_argument("--subindex", type=integer, default=0, help="the subindex of the object")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(AlState.PREOP, positions=[args.slave])

    data = client.read_sdo(args.slave, args.index, args.subindex)

    value = int.from_bytes(data, "little")
    print(
        f"slave {args.slave} {object_address_text(args.index, args.subindex)} = "
        f"{value} (0x{value:x}), {len(data)} byte(s): {list(data)}"
    )


if __name__ == "__main__":
    run(main)
