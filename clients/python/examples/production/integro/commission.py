#!/usr/bin/env python3
"""Commission an Integro drive, step by step, and keep a record of what was done.

Run it in the directory that holds the firmware package and the configuration file. The log and
the record go to logs/ beside this script.

    python commission.py
    python commission.py --slave 2
"""

import argparse
import logging
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common import Exit, Session  # noqa: E402
from motion_master_client import run  # noqa: E402

log = logging.getLogger("production")

HARDWARE_DESCRIPTION_WAIT_S = 10


def product_lines(title, part):
    """Return the lines that describe one part of a hardware description."""
    lines = [
        f"{title}: {part['name']}",
        f"  build descriptor {part['buildDescriptor']}, serial number {part['serialNumber']}",
    ]
    for c in part.get("components", []):
        version = f", version {c['version']}" if c.get("version") else ""
        lines.append(f"  - {c['name']}, serial number {c['serialNumber']}{version}")
    return lines


def read_hardware_description_file(s):
    """Read the hardware description, and ask again while the drive refuses the FoE read.

    After a power cycle, an Integro refuses FoE reads for about 2.5 s after it reaches PRE-OP,
    while SDO works (#175). Each refusal is logged.
    """
    deadline = time.monotonic() + HARDWARE_DESCRIPTION_WAIT_S
    path = {"slavePosition": s.slave}
    while True:
        r = s.client.call("getDeviceHardwareDescription", path=path, check=False)
        if r.ok or time.monotonic() >= deadline:
            r.raise_for_status()
            return r.json()
        log.info("the drive refused the read, asking again: %s", r.json().get("error", r.text))
        time.sleep(0.5)


def read_hardware_description(s):
    """Step 1: read the hardware description and the firmware version."""
    s.heading(1, "hardware description")
    hd = read_hardware_description_file(s)
    lines = product_lines("device", hd["device"])
    if hd.get("assembly"):
        lines += product_lines("assembly", hd["assembly"])
    firmware = s.client.call(
        "readParameter", path={"slavePosition": s.slave, "index": 0x100A, "subindex": 0}
    ).json()["value"]
    lines.append(f"firmware version: {firmware}")

    for line in lines:
        log.info(line)
    device = hd["device"]
    s.identify(device["name"], f"{device['buildDescriptor']}_{device['serialNumber']}")
    s.add("hardware description", lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    args = ap.parse_args()

    s = Session(args.slave, Path(__file__).resolve().parent / "logs")
    outcome = "failed"
    try:
        s.connect()
        read_hardware_description(s)
        outcome = "completed"
    except Exit as e:
        outcome = f"stopped: {e}"
        log.info("stopped: %s", e)
    finally:
        s.write_record(outcome)


if __name__ == "__main__":
    run(main)
