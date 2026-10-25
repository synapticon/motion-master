#!/usr/bin/env python3
"""Print the PDO mapping of a device, grouped by PDO (GET .../pdo-mapping).

The server reads the mapping from the object dictionary of the device: 0x1C12 and 0x1C13 name the
assigned PDOs, and each PDO object lists the objects it maps. The script prints each entry with
its bit offset from the start of the outputs or the inputs of the device, its bit length and its
name. --output also saves the mapping as JSON, in the shape that write-pdo-mapping.py takes.

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. A device in
SAFE-OP or OP stays there.

    python read-pdo-mapping.py
    python read-pdo-mapping.py --slave 2
    python read-pdo-mapping.py --output files/pdo-mapping.json
"""

import argparse
import json
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import PRE_OP, MotionMasterClient, run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--output", help="also save the mapping as JSON to this path")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(PRE_OP, positions=[args.slave])

    mapping = client.call("getDevicePdoMapping", path={"slavePosition": args.slave}).json()
    parameters = client.call("getDeviceParameters", path={"slavePosition": args.slave}).json()
    names = {(p["index"], p["subindex"]): p["name"] for p in parameters}

    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(mapping, indent=2))
        print(f"saved -> {out}")

    for direction in ("outputs", "inputs"):
        print(f"{direction}:")
        for pdo in mapping[direction]:
            print(f"  0x{pdo['pdoIndex']:04x}")
            for e in pdo["entries"]:
                name = names.get((e["index"], e["subindex"]), "")
                print(
                    f"    bit {e['bitOffset']:>4}  {e['bitLength']:>2} bit(s)  "
                    f"0x{e['index']:04x}:{e['subindex']:02x}  {name}"
                )


if __name__ == "__main__":
    run(main)
