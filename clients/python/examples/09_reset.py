#!/usr/bin/env python3
"""Reset the fieldbus driver and clear the device list (POST /api/reset).

Tears down all master-side state -- unpublishes the process image, frees the
recorder, drops every Device, and closes the raw socket (releasing the NIC) --
returning Motion Master to the uninitialised state it started in. It does not
command the slaves to any AL state; the master simply stops talking to the bus.
After this, 01_init and 02_scan can be run again, closing the lifecycle.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common import make_client  # noqa: E402


def main():
    client = make_client()
    client.call("reset")
    print("reset -- driver torn down, device list cleared")


if __name__ == "__main__":
    main()
