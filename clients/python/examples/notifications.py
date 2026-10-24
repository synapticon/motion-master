#!/usr/bin/env python3
"""Print the notifications that the server sends over the WebSocket, until Ctrl-C.

Every notification source of the server publishes to one topic, "notifications", so one
subscription receives every kind of event, also a kind that a later server adds. data.event names
the kind. The bus health source, for example, sends "short-working-counter" when cycles came back
with a short working counter.

The script needs no fieldbus driver, so it opens none and leaves the server as it is. A source
that has nothing to report sends nothing, so on a healthy bus the script can print nothing.

    python notifications.py
    python notifications.py --count 5
"""

import argparse
import json
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import MotionMasterClient  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--count", type=int, default=0, help="stop after N notifications (0 = until Ctrl-C)"
    )
    args = ap.parse_args()

    client = MotionMasterClient.from_config()
    # Each line is flushed, because notifications are rare and output into a pipe is buffered.
    print("subscribed to 'notifications' -- Ctrl-C to stop", flush=True)
    received = 0
    try:
        for data in client.notifications():
            ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            event = data.get("event", "?")
            rest = {k: v for k, v in data.items() if k != "event"}
            print(f"  {ts}  {event}  {json.dumps(rest, ensure_ascii=False)}", flush=True)
            received += 1
            if args.count and received >= args.count:
                break
    except KeyboardInterrupt:
        print("\nstopping")


if __name__ == "__main__":
    main()
