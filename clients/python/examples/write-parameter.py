#!/usr/bin/env python3
"""Write one parameter, converted to its data type (PUT .../parameters/{index}/{subindex}).

The script reads the parameter first, and converts --value by the JSON type of its current value:
an integer as decimal or 0x-prefixed hex, a real number as a float, a string as written, and a
byte array as bytes separated by commas. The server then converts the value to the declared data
type, and refuses one that the type cannot hold. It sends the value in the process data when the
object is output-mapped and the device exchanges process data, and with an SDO write otherwise.
The script prints the value before and after.

A write to a parameter that controls the drive, such as a target, takes effect at once. Run
store-parameters.py to keep a value across a power cycle.

Opens the fieldbus driver, scans and moves the device to PRE-OP first when needed. A device in
SAFE-OP or OP stays there.

    python write-parameter.py --index 0x6060 --subindex 0 --value 8
    python write-parameter.py --index 0x6060 --subindex 0 --value 10 --slave 2
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import PRE_OP, MotionMasterClient  # noqa: E402


def auto_int(text):
    """Parse a decimal or 0x-prefixed hex integer."""
    return int(text, 0)


def convert(current, text):
    """Convert the text of --value to the JSON type of the current value of the parameter."""
    if isinstance(current, bool) or isinstance(current, int):
        return int(text, 0)
    if isinstance(current, float):
        return float(text)
    if isinstance(current, list):
        return [int(b, 0) for b in text.split(",")]
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--index", type=auto_int, required=True)
    ap.add_argument("--subindex", type=auto_int, required=True)
    ap.add_argument("--value", required=True)
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(PRE_OP, positions=[args.slave])

    path = {"slavePosition": args.slave, "index": args.index, "subindex": args.subindex}
    before = client.call("readParameter", path=path).json()
    try:
        value = convert(before["value"], args.value)
    except ValueError as e:
        sys.exit(f"--value {args.value!r} does not fit {before['dataTypeName']}: {e}")

    r = client.call("writeParameter", path=path, body={"value": value}, check=False)
    if r.status_code != 200:
        sys.exit(r.json().get("error", r.text))
    after = r.json()
    print(
        f"slave {args.slave} 0x{args.index:04x}:{args.subindex:02x} {after['name']} "
        f"({after['dataTypeName']}): {before['value']!r} -> {after['value']!r}"
    )


if __name__ == "__main__":
    main()
