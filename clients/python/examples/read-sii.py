#!/usr/bin/env python3
"""Read the SII of a device, the configuration in its EEPROM (GET .../sii).

By default the script prints the parsed SII as JSON. `info` holds the identity, the mailbox
layout and the PDI configuration. `category` holds the strings, the general category, the FMMUs,
the sync managers, the PDOs and the distributed clock settings. --output saves the raw EEPROM
image instead, which write-sii.py and POST /api/sii/parse take. The server reads the EEPROM live,
in every AL state.

Opens the fieldbus driver and scans first when needed, and moves no device.

    python read-sii.py
    python read-sii.py --slave 2 --output files/sii.bin
"""

import argparse
import json
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import MotionMasterClient, run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--output", help="save the raw EEPROM image to this path")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_scanned()

    path = {"slavePosition": args.slave}
    if args.output:
        # The Accept header selects the raw image over the parsed JSON.
        content = client.call(
            "readSii", path=path, headers={"Accept": "application/octet-stream"}
        ).content
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(content)
        print(f"read {len(content)} byte(s) of SII from slave {args.slave} -> {out}")
        return

    print(json.dumps(client.call("readSii", path=path).json(), indent=2, ensure_ascii=False))


if __name__ == "__main__":
    run(main)
