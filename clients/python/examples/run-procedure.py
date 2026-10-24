#!/usr/bin/env python3
"""List the procedures of a device, describe one, or run one by name with its parameters.

With no name, the script lists every procedure the device has (GET .../procedures). With a name
and --describe, it prints what the procedure does, its caveats and its parameters. With a name,
it runs the procedure (POST .../procedures/{name}), prints each step when its status changes, and
exits with 1 unless the run succeeds. Ctrl-C cancels the run.

Parameters go on the command line as name=value. The script converts each value by the type the
descriptor of the procedure gives:

    integer       decimal or 0x-prefixed hex
    boolean       true or false
    enum          one of the option values that --describe lists
    byteArray     bytes separated by commas, each decimal or 0x-prefixed hex
    string        as written
    stringArray   values separated by commas
    file          a local path, which the script reads and sends as base64

Opens the fieldbus driver, scans and moves the device to --state first when needed. The default
is PRE-OP, because most procedures need the mailbox. A procedure that enables the drive needs OP,
because the drive enables only while it exchanges process data. Give --state op for it.

    python run-procedure.py
    python run-procedure.py store-parameters --describe
    python run-procedure.py restore-default-parameters group=application
    python run-procedure.py os-command command=0x15,0,0,0,0,0,0,0 fsBuffer=read
    python run-procedure.py offset-detection --state op --slave 2
"""

import argparse
import base64
import json
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import STATE_NAMES, MotionMasterClient  # noqa: E402

STATES = {name.lower(): value for value, name in STATE_NAMES.items() if value != 0}


def convert(param, text):
    """Convert the text of one name=value argument to the JSON value of its parameter type."""
    kind = param["type"]
    if kind == "integer":
        return int(text, 0)
    if kind == "boolean":
        if text.lower() not in ("true", "false"):
            raise ValueError("expected true or false")
        return text.lower() == "true"
    if kind == "enum":
        for option in param["options"]:
            if str(option["value"]) == text:
                return option["value"]
        values = ", ".join(str(o["value"]) for o in param["options"])
        raise ValueError(f"expected one of {values}")
    if kind == "byteArray":
        return [int(b, 0) for b in text.split(",")]
    if kind == "stringArray":
        return text.split(",") if text else []
    if kind == "file":
        return base64.b64encode(Path(text).read_bytes()).decode("ascii")
    return text


def build_params(descriptor, assignments):
    """Build the request body from name=value arguments, checked against the descriptor."""
    params = {p["name"]: p for p in descriptor["parameters"]}
    body = {}
    for assignment in assignments:
        name, sep, text = assignment.partition("=")
        if not sep:
            sys.exit(f"{assignment!r}: write a parameter as name=value")
        if name not in params:
            known = ", ".join(params) or "none"
            sys.exit(f"{descriptor['name']} has no parameter {name!r} (parameters: {known})")
        try:
            body[name] = convert(params[name], text)
        except (ValueError, OSError) as e:
            sys.exit(f"{name}: {e}")
    missing = [n for n, p in params.items() if p["required"] and n not in body]
    if missing:
        sys.exit(f"{descriptor['name']} needs {', '.join(missing)}")
    return body


def flags(descriptor):
    """Return the flags of a procedure in parentheses, worded as the Console shows them."""
    words = []
    if descriptor["movesMotor"]:
        words.append("can move the motor")
    if descriptor["requiresEnabled"]:
        words.append("requires the drive enabled")
    return f" ({', '.join(words)})" if words else ""


def describe(descriptor):
    """Print what a procedure does, its caveats and its parameters."""
    print(f"{descriptor['name']}: {descriptor['title']}{flags(descriptor)}")
    print(f"\n{descriptor['description']}")
    for caveat in descriptor["caveats"]:
        print(f"\n- {caveat}")
    if descriptor["parameters"]:
        print("\nparameters:")
    for p in descriptor["parameters"]:
        if p["required"]:
            spec = "required"
        else:
            spec = f"default {json.dumps(p.get('defaultValue'))}"
        if p["type"] == "enum":
            spec += "; " + ", ".join(f"{o['value']} ({o['title']})" for o in p["options"])
        print(f"  {p['name']} ({p['type']}, {spec})")
        print(f"      {p['description']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name", nargs="?", help="the procedure to run (default: list them)")
    ap.add_argument("params", nargs="*", help="parameters as name=value")
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--state", choices=STATES, default="pre-op")
    ap.add_argument("--describe", action="store_true", help="describe the procedure, do not run it")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(STATES[args.state], positions=[args.slave])

    procedures = client.call("listProcedures", path={"slavePosition": args.slave}).json()
    descriptors = {p["descriptor"]["name"]: p["descriptor"] for p in procedures}

    if args.name is None:
        for d in descriptors.values():
            print(f"  {d['name']:<32} {d['title']}{flags(d)}")
        return

    descriptor = descriptors.get(args.name)
    if descriptor is None:
        sys.exit(f"slave {args.slave} has no procedure {args.name!r}")
    if args.describe:
        describe(descriptor)
        return

    body = build_params(descriptor, args.params)

    # A step that starts and ends between two polls shows only once, already finished, so the
    # script prints each change of status rather than each poll. A step not reached yet is idle,
    # and stays unprinted until it starts.
    printed = {}
    snapshot = None
    run = client.run_procedure(args.slave, args.name, body)
    try:
        for snapshot in run:
            for step in snapshot["steps"]:
                if printed.get(step["id"], "idle") == step["status"]:
                    continue
                printed[step["id"]] = step["status"]
                line = f"  {step['id']:<36} {step['status']}"
                if "value" in step:
                    line += f"  {json.dumps(step['value'], ensure_ascii=False)}"
                if "error" in step:
                    line += f"  {step['error']}"
                print(line)
    except KeyboardInterrupt:
        print("\ncancelled")
        sys.exit(1)
    finally:
        # Closing the generator cancels a run that is still in progress.
        run.close()

    if snapshot.get("error"):
        print(f"error: {snapshot['error']}")
    print(f"{args.name} {snapshot['status']}")
    if snapshot["status"] != "succeeded":
        sys.exit(1)


if __name__ == "__main__":
    main()
