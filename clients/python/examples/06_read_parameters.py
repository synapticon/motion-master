#!/usr/bin/env python3
"""Enumerate a device's full object dictionary with values and print it.

POST /api/devices/{slavePosition}/parameters/init?readValues=true walks the
entire CoE object dictionary via the SDO Info service and, with readValues=true,
also SDO-reads and decodes every entry's value in the same call. The device must
be in PRE-OP or above (mailbox active), so run 03_transition_preop first. On a
fully populated drive this can take several seconds.

    python 06_read_parameters.py --slave 1
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common import make_client  # noqa: E402


def decode_value(param):
    """Render a DeviceParameter value, decoding raw byte arrays by data type.

    The server already decodes the standard scalar and string types (they arrive
    as JSON numbers/strings). What comes back as a byte array is everything the
    server left raw: the odd-width integers (INTEGER24/40/48/56, UNSIGNED..),
    ENUM, BIT*/BITARR*, OCTET_STRING, GUID, DOMAIN, RECORD, etc. We decode the
    ones with a well-defined scalar interpretation and show hex for the rest.
    """
    value = param["value"]
    if not isinstance(value, list):
        return repr(value)  # already a decoded number or string

    name = param["dataTypeName"]
    raw = bytes(value)
    hexs = raw.hex(" ")

    if name.startswith("INTEGER"):
        return f"{int.from_bytes(raw, 'little', signed=True)}  (0x {hexs})"
    if name.startswith(("UNSIGNED", "BIT", "ENUM")) or name in ("BYTE", "WORD", "DWORD"):
        return f"{int.from_bytes(raw, 'little')}  (0x {hexs})"
    if name == "OCTET_STRING":
        text = raw.rstrip(b"\x00 ").decode("ascii", "replace")
        return f"{text!r}  (0x {hexs})"
    return f"0x {hexs}" if raw else "(empty)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    args = ap.parse_args()

    client = make_client()
    print(f"enumerating object dictionary of slave {args.slave} (this may take a few seconds)...")

    params = client.call(
        "initializeDeviceParameters",
        path={"slavePosition": args.slave},
        query={"readValues": "true"},
    ).json()

    for p in params:
        print(
            f"  0x{p['index']:04x}:{p['subindex']:02x}  "
            f"{p['dataTypeName']:<14}  {decode_value(p):<24}  {p['name']}"
        )
    print(f"{len(params)} parameter(s)")


if __name__ == "__main__":
    main()
