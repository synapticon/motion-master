#!/usr/bin/env python3
"""Parse an ESI file and list the devices it describes (POST /api/esi/parse).

An ESI (EtherCAT Slave Information) is the XML file in which a vendor describes its devices. The
server parses it offline and returns every device with its own object table, one entry per index
and subindex, with the objects of its modules merged in. The script prints a line per device.
--output saves the whole response as JSON, entries and warnings included.

The script needs no bus and no fieldbus driver, so it opens none and leaves the server as it is.

    python parse-esi.py ../../libs/etg/tests/data/somanet-v5.6.6.xml
    python parse-esi.py somanet.xml --output files/esi.json
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import MotionMasterClient, run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("esi", help="path to the ESI XML file")
    ap.add_argument("--output", help="save the whole parsed response as JSON to this path")
    args = ap.parse_args()

    client = MotionMasterClient.from_config()
    result = client.call(
        "parseEsi",
        data=Path(args.esi).read_bytes(),
        headers={"Content-Type": "application/xml"},
    ).json()

    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(result, indent=2, ensure_ascii=False))
        print(f"saved -> {out}")

    vendor = result["vendor"]
    print(f"{vendor['name']} (vendor 0x{vendor['id']:08x}), ESI version {result['version']}")
    for d in result["devices"]:
        print(
            f"  {d['name']}: product 0x{d['productCode']:08x}, revision 0x{d['revisionNo']:08x}, "
            f"{d['objectCount']} object(s) of its own, {len(d['entries'])} entries with its "
            f"modules, {d['rxPdoCount']} RxPDO(s), {d['txPdoCount']} TxPDO(s), "
            f"{len(d['warnings'])} warning(s)"
        )


if __name__ == "__main__":
    run(main)
