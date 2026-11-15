#!/usr/bin/env python3
"""Write raw bytes to one object with an SDO download (PUT .../sdo/{index}/{subindex}).

--data gives the bytes in wire order, little-endian, separated by commas. The byte count must
match the length of the object. write-parameter.py converts a value to the data type instead,
which is the easier way for most objects.

Opens the fieldbus driver, scans and moves the device to PREOP first when needed, because the
mailbox is live from PREOP up. A device in SAFEOP or OP stays there.

    python write-sdo.py --index 0x6060 --subindex 0 --data 8
    python write-sdo.py --index 0x607a --subindex 0 --data 0x10,0x27,0,0 --slave 2
"""

import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import AlState, MotionMasterClient, object_address_text, run  # noqa: E402
from options import byte_list, integer, parser  # noqa: E402


def main():
    ap = parser(__doc__)
    ap.add_argument(
        "--slave", type=integer, default=1, help="the position of the device on the bus"
    )
    ap.add_argument("--index", type=integer, required=True, help="the index of the object")
    ap.add_argument("--subindex", type=integer, required=True, help="the subindex of the object")
    ap.add_argument(
        "--data",
        type=byte_list,
        required=True,
        help="bytes separated by commas, such as 0x10,0x27",
    )
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(AlState.PREOP, positions=[args.slave])

    client.write_sdo(args.slave, args.index, args.subindex, args.data)
    address = object_address_text(args.index, args.subindex)
    print(f"slave {args.slave} {address} <- {bytes(args.data).hex(' ').upper()}")


if __name__ == "__main__":
    run(main)
