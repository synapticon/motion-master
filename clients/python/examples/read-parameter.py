#!/usr/bin/env python3
"""Read one parameter with its name, data type and value (GET .../parameters/{index}/{subindex}).

The server reads the value from the process image when the object is PDO-mapped and the device
exchanges process data, and with an SDO read otherwise. --cache returns the value the server holds
and touches no bus. read-sdo.py reads the raw bytes of an object instead, with no decoding.

Opens the fieldbus driver, scans and moves the device to PREOP first when needed. The server
learns the parameters of a device when it reaches PREOP. A device in SAFEOP or OP stays there.

The defaults read the Statusword, 0x6041:00.

    python read-parameter.py
    python read-parameter.py --index 0x6064 --subindex 0 --slave 2
    python read-parameter.py --index 0x2031 --subindex 1 --cache
"""

import json
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
    ap.add_argument("--cache", action="store_true", help="return the cached value, read no bus")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(AlState.PREOP, positions=[args.slave])

    p = client.call(
        "readParameter",
        path={"slavePosition": args.slave, "index": args.index, "subindex": args.subindex},
        query={"source": "cache"} if args.cache else None,
    ).json()
    print(
        f"slave {args.slave} {object_address_text(p['index'], p['subindex'])} {p['name']} "
        f"({p['dataTypeName']}) = {json.dumps(p['value'], ensure_ascii=False, separators=(",", ":"))}"
    )


if __name__ == "__main__":
    run(main)
