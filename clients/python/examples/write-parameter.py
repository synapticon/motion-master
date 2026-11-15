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

Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
SAFEOP or OP stays there.

    python write-parameter.py --index 0x6060 --subindex 0 --value 8
    python write-parameter.py --index 0x6060 --subindex 0 --value 10 --slave 2
"""

import json
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import AlState, MotionMasterClient, object_address_text, run  # noqa: E402
from options import byte_list, integer, parser  # noqa: E402


def convert(current, text):
    """Convert the text of --value to the JSON type of the current value of the parameter."""
    if isinstance(current, bool) or isinstance(current, int):
        return integer(text)
    if isinstance(current, float):
        return float(text)
    if isinstance(current, list):
        return byte_list(text)
    return text


def main():
    ap = parser(__doc__)
    ap.add_argument(
        "--slave", type=integer, default=1, help="the position of the device on the bus"
    )
    ap.add_argument("--index", type=integer, required=True, help="the index of the object")
    ap.add_argument("--subindex", type=integer, required=True, help="the subindex of the object")
    ap.add_argument(
        "--value", required=True, help="the value, converted to the data type of the parameter"
    )
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(AlState.PREOP, positions=[args.slave])

    path = {"slavePosition": args.slave, "index": args.index, "subindex": args.subindex}
    before = client.call("readParameter", path=path).json()
    try:
        value = convert(before["value"], args.value)
    except ValueError as e:
        sys.exit(f"--value {args.value!r} does not fit {before['dataTypeName']}: {e}")

    after = client.write_value(args.slave, (args.index, args.subindex), value)
    print(
        f"slave {args.slave} {object_address_text(args.index, args.subindex)} {before['name']} "
        f"({before['dataTypeName']}): {_json(before['value'])} -> {_json(after)}",
    )


def _json(value):
    """Return a value as JSON, as the TypeScript example prints it."""
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


if __name__ == "__main__":
    run(main)
