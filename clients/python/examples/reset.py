#!/usr/bin/env python3
"""Close the fieldbus driver and clear the device list (POST /api/reset).

The server unpublishes the process image, frees the recorder, removes every device and closes
the network interface. It commands no AL state, so the devices stay in the state they are in.
The server then is as it was at start, with no driver open.

A reset with no driver open also succeeds, so the script runs again with the same result. The
next script that needs a bus opens the driver and scans again.

    python reset.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import MotionMasterClient, run  # noqa: E402


def main():
    client = MotionMasterClient.from_config()
    client.call("reset")
    print("reset: the fieldbus driver is closed and the device list is empty")


if __name__ == "__main__":
    run(main)
