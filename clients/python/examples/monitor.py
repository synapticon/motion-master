#!/usr/bin/env python3
"""Stream live process data over the monitoring WebSocket.

Creates a monitoring over the HTTP API (POST /api/monitorings), subscribes to its topic on the
WebSocket, and prints each batch of cycle rows as it arrives. The monitoring is deleted on exit.

Opens the fieldbus driver, scans and moves the device to OP first when needed. A value is null
while its device is not exchanging process data, which starts in SAFE-OP.

The defaults sample four objects of the device: Timestamp (0x20F0:00), Position actual value
(0x6064:00), DC link circuit voltage (0x6079:00) and Drive temperature (0x2031:01). The server
reads each object from the process image when it is PDO-mapped, and with a background SDO poll
otherwise. The header line shows which, as (pdo) or (sdo).

    python monitor.py --count 10
    python monitor.py --slave 2 --interval 500     # runs until Ctrl-C
"""

import argparse
import logging
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import OP, MotionMasterClient  # noqa: E402

TOPIC = "python-example"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--interval", type=int, default=100, help="flush cadence in ms (5-2000)")
    ap.add_argument("--count", type=int, default=0, help="stop after N batches (0 = until Ctrl-C)")
    args = ap.parse_args()

    # Each parameter is [devicePosition, index, subindex].
    parameters = [
        [args.slave, 0x20F0, 0],  # Timestamp
        [args.slave, 0x6064, 0],  # Position actual value
        [args.slave, 0x6079, 0],  # DC link circuit voltage
        [args.slave, 0x2031, 1],  # Drive temperature
    ]

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(OP, positions=[args.slave])

    # A monitoring with this topic can remain from a run that was killed, and the server refuses
    # a second create of one topic with 409.
    client.call("deleteMonitoring", path={"topic": TOPIC}, check=False)
    client.call(
        "createMonitoring",
        body={"topic": TOPIC, "interval": args.interval, "parameters": parameters},
    )

    order = client.call("getMonitoring", path={"topic": TOPIC}).json()["parameters"]
    header = ", ".join(f"0x{p['index']:04x}:{p['subindex']:02x}({p['source']})" for p in order)
    print(f"subscribed to '{TOPIC}' [{header}] -- Ctrl-C to stop")

    batches = 0
    try:
        for rows in client.monitor(TOPIC):
            ts_us, *values = rows[-1]
            ts = datetime.fromtimestamp(ts_us / 1e6, timezone.utc).strftime("%H:%M:%S.%f")
            print(f"  {ts}  {len(rows):4d} row(s)  latest={values}")
            batches += 1
            if args.count and batches >= args.count:
                break
    except KeyboardInterrupt:
        print("\nstopping")
    finally:
        client.call("deleteMonitoring", path={"topic": TOPIC}, check=False)


if __name__ == "__main__":
    main()
