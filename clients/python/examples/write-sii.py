#!/usr/bin/env python3
"""Write a raw SII image to the EEPROM of a device (PUT /api/devices/{slavePosition}/sii).

The SII (Slave Information Interface) holds the identity and the configuration of the device.
The server parses the image first and refuses one that is not a well-formed SII. A wrong image
that parses can still leave the device unidentifiable until it is flashed again, so write only an
image you read from the same kind of device, with read-sii.py --output. The device adopts the
new SII only after a power cycle.

Opens the fieldbus driver and scans first when needed, and moves no device. The write is most
reliable in INIT or PRE-OP.

    python write-sii.py --file files/sii.bin
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import MotionMasterClient  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument(
        "--file", required=True, help="the raw SII image, as read-sii.py --output saves it"
    )
    args = ap.parse_args()
    image = Path(args.file).read_bytes()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_scanned()

    r = client.call(
        "writeSii",
        path={"slavePosition": args.slave},
        data=image,
        headers={"Content-Type": "application/octet-stream"},
        check=False,
    )
    if r.status_code != 200:
        sys.exit(r.json().get("error", r.text))
    print(f"wrote {len(image)} byte(s) of SII to slave {args.slave}. Power cycle it to apply.")


if __name__ == "__main__":
    main()
