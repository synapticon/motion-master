#!/usr/bin/env python3
"""Scan the bus for slaves (POST /api/scan).

Must be called after 01_init. Slaves are left in INIT state; an empty bus is a
successful scan returning 0 slaves, not an error.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common import make_client  # noqa: E402


def main():
    client = make_client()
    slaves = client.call("scan").json()["slaves"]
    print(f"scan found {slaves} slave(s)")


if __name__ == "__main__":
    main()
