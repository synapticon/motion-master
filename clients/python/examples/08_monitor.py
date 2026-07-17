#!/usr/bin/env python3
"""Stream live process data over the monitoring WebSocket.

Registers a monitoring over the HTTP API (POST /api/monitorings), then subscribes
to its topic on the WebSocket (wss://host:62281) and prints each batch of cycle
rows as they arrive.

Monitoring is live-only: a value is `null` until its device is exchanging
(SAFE-OP/OP), so run 07_transition_op first for non-null data. By default this
samples slave 1's Statusword (0x6041:00) and Position actual value (0x6064:00).

    python 08_monitor.py --slave 1 --interval 100 --count 10
    python 08_monitor.py                 # runs until Ctrl-C
"""

import argparse
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common import make_client  # noqa: E402

TOPIC = "python-example"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--interval", type=int, default=100, help="flush cadence in ms (5-2000)")
    ap.add_argument("--count", type=int, default=0, help="stop after N batches (0 = until Ctrl-C)")
    args = ap.parse_args()

    # Each parameter is [devicePosition, index, subindex].
    parameters = [
        [args.slave, 0x6041, 0],  # Statusword
        [args.slave, 0x6064, 0],  # Position actual value
    ]

    client = make_client()

    # Recreate cleanly in case a monitoring with this topic lingers from a prior run.
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
            ts_us, *values = rows[-1]  # freshest row in the batch
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
