#!/usr/bin/env python3
"""Write the PDO mapping of a device from a JSON file (PUT .../pdo-mapping).

The file has the shape that read-pdo-mapping.py --output saves: "outputs" lists the RxPDO
objects (0x16xx) and "inputs" the TxPDO objects (0x1Axx), each with its entries in order. The
order of the objects is the order of the sync manager assignment. An empty list clears that
direction. The script ignores the bitOffset of an entry, because the device derives the offsets.

The mapping objects are writable only in PRE-OP, so the script restarts the device through INIT
to PRE-OP, writes the mapping, and moves the device back to SAFE-OP or OP when it was there. The
server then maps the process image of the whole bus again from the new mapping, which pauses the
bus briefly. The script prints the mapping that the device reports back.

Opens the fieldbus driver and scans first when needed.

    python read-pdo-mapping.py --output files/pdo-mapping.json
    python write-pdo-mapping.py --file files/pdo-mapping.json
"""

import argparse
import json
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import OP, PRE_OP, SAFE_OP, MotionMasterClient, run  # noqa: E402


def request_objects(objects):
    """Return the PDO objects of one direction without the bit offsets that the device derives."""
    return [
        {
            "pdoIndex": o["pdoIndex"],
            "entries": [
                {"index": e["index"], "subindex": e["subindex"], "bitLength": e["bitLength"]}
                for e in o["entries"]
            ],
        }
        for o in objects
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--file", required=True, help="the mapping as JSON")
    args = ap.parse_args()
    mapping = json.loads(Path(args.file).read_text())
    body = {
        "outputs": request_objects(mapping["outputs"]),
        "inputs": request_objects(mapping["inputs"]),
    }

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_scanned()

    query = {"positions": str(args.slave)}
    previous = client.call("getDeviceStates", query=query).json()[0]["alState"]

    # Through INIT, never down from SAFE-OP or OP. On an Integro, a write of the mapping objects
    # aborts with 0x08000000 after SAFE-OP -> PRE-OP or OP -> PRE-OP, and passes after
    # INIT -> PRE-OP. The firmware source points at its application processor, which learns only
    # of a transition upwards and so checks the write against the rights of the state before.
    restart = client.call("restartDevices", body={"positions": [args.slave], "target": PRE_OP})
    if not restart.json()["ok"]:
        sys.exit(f"slave {args.slave} did not reach PRE-OP through INIT")

    r = client.call(
        "writeDevicePdoMapping", path={"slavePosition": args.slave}, body=body, check=False
    )
    # A failed write is not undone, so the device stays in PRE-OP, where its mapping can be read and
    # written again.
    if r.status_code != 200:
        sys.exit(f"{r.json().get('error', r.text)}\nslave {args.slave} stays in PRE-OP")
    read_back = r.json()

    # A device that came from below PRE-OP stays in PRE-OP, which is where its mapping applies.
    if previous in (SAFE_OP, OP):
        client.ensure_state(previous, positions=[args.slave], exact=True)

    for direction in ("outputs", "inputs"):
        print(f"{direction}:")
        for pdo in read_back[direction]:
            entries = ", ".join(
                f"0x{e['index']:04x}:{e['subindex']:02x}/{e['bitLength']}" for e in pdo["entries"]
            )
            print(f"  0x{pdo['pdoIndex']:04x}  {entries}")


if __name__ == "__main__":
    run(main)
