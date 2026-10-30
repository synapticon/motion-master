#!/usr/bin/env python3
"""Record the EtherCAT frames on the bus adapter for some seconds, and print the summary.

POST /api/captures runs motion-master-capture on the adapter that the bus is on, waits for it to
end, and answers with the path of the pcap file and its summary. The summary counts, for each kind
of frame, how many came back later than 2 ms, 20 ms and 100 ms. The request blocks for the whole
capture. The server runs one capture at a time.

Opens the fieldbus driver first when none is open, because the server records the adapter of the
open driver. It scans nothing and moves no device.

    python capture.py
    python capture.py --seconds 30
"""

import argparse
import json
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import MotionMasterClient, run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=10, help="capture length, 1 to 300")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()

    print(f"recording for {args.seconds} s...")
    result = client.call("runCapture", body={"seconds": args.seconds}).json()
    print(f"pcap on the server: {result['path']}")
    print(json.dumps(result["summary"], indent=2))


if __name__ == "__main__":
    run(main)
