#!/usr/bin/env python3
"""Read the whole object dictionary of a device with its values and print it.

POST /api/devices/{slavePosition}/parameters/init?readValues=true reads the object dictionary
through the SDO Information service. With readValues=true it also reads and decodes the value of
every entry in the same call. On a drive with a full dictionary this takes several seconds.

Opens the fieldbus driver, scans and moves the device to PREOP first when needed, because the
mailbox is live from PREOP up. A device in SAFEOP or OP stays there.

    python read-all-parameters.py
    python read-all-parameters.py --slave 2
"""

import argparse
import json
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import AlState, MotionMasterClient, run  # noqa: E402


def decode_value(param):
    """Render a DeviceParameter value, and decode a raw byte array by its data type.

    The server decodes the standard scalar and string types, which arrive as JSON numbers and
    strings. Every other type arrives as a byte array: the odd-width integers, ENUM, the BIT types,
    OCTET_STRING, GUID, DOMAIN and RECORD. The types with one scalar reading are decoded here. The
    rest is shown as hex.
    """
    value = param["value"]
    if not isinstance(value, list):
        # As the TypeScript example prints it: a string as JSON, "v5.6.11", and a whole float with
        # no fraction, 0 and not 0.0.
        if isinstance(value, float) and value.is_integer():
            value = int(value)
        return json.dumps(value, ensure_ascii=False)

    name = param["dataTypeName"]
    raw = bytes(value)
    hexs = raw.hex(" ")

    if name.startswith("INTEGER"):
        return f"{int.from_bytes(raw, 'little', signed=True)}  (0x {hexs})"
    if name.startswith(("UNSIGNED", "BIT", "ENUM")) or name in ("BYTE", "WORD", "DWORD"):
        return f"{int.from_bytes(raw, 'little')}  (0x {hexs})"
    if name == "OCTET_STRING":
        text = raw.rstrip(b"\x00 ").decode("ascii", "replace")
        return f"{json.dumps(text, ensure_ascii=False)}  (0x {hexs})"
    return f"0x {hexs}" if raw else "(empty)"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--slave", type=int, default=1)
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(AlState.PREOP, positions=[args.slave])

    print(f"reading the object dictionary of slave {args.slave}...")
    params = client.call(
        "initializeDeviceParameters",
        path={"slavePosition": args.slave},
        query={"readValues": "true"},
    ).json()

    for p in params:
        print(
            f"  0x{p['index']:04X}:{p['subindex']:02X}  "
            f"{p['dataTypeName']:<14}  {decode_value(p):<24}  {p['name']}"
        )
    print(f"{len(params)} parameter(s)")


if __name__ == "__main__":
    run(main)
