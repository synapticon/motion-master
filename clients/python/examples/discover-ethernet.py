#!/usr/bin/env python3
"""Find the drives on an Ethernet adapter of the server and list them (GET /api/ethernet/discovery).

The server sends a PROFINET DCP Identify All and an EtherNet/IP ListIdentity request on the
adapter, collects the answers, and listens for LLDP during that time. It opens no connection to a
drive. A drive sends LLDP only about every 30 s, so --duration-ms 35000 also finds every drive that
only LLDP finds. The discovery needs no open fieldbus driver.

    python discover-ethernet.py --adapter spoe0
    python discover-ethernet.py --adapter spoe0 --duration-ms 35000
"""

import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import MotionMasterClient, run  # noqa: E402
from options import integer, parser  # noqa: E402


def main():
    ap = parser(__doc__)
    ap.add_argument("--adapter", required=True, help="name or MAC address of the adapter")
    ap.add_argument(
        "--duration-ms", type=integer, default=2000, help="how long to collect answers, in ms"
    )
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()

    result = client.discover_ethernet_devices(args.adapter, duration_ms=args.duration_ms)
    for d in result["devices"]:
        print(
            f"  {d['ipAddress'] or '-':<15}  {d['macAddress'] or '-':<17}"
            f"  {d['firmware'] or '-':<11}  {d['name']:<20}  found by {', '.join(d['foundBy'])}"
        )
    for warning in result["warnings"]:
        print(f"warning: {warning}")


if __name__ == "__main__":
    run(main)
