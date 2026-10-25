#!/usr/bin/env python3
"""Print the server version, the host, the network adapters and the game loop health.

Reads GET /api/version, GET /api/system-info, GET /api/adapters and GET /api/game-loop. The
script needs no fieldbus driver, so it opens none and leaves the server as it is.

    python server-info.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import MotionMasterClient, run  # noqa: E402


def main():
    client = MotionMasterClient.from_config()

    version = client.call("getVersion").json()["version"]
    info = client.call("getSystemInfo").json()
    print(f"Motion Master {version}")
    print(f"  host     {info['hostname']}: {info['osName']}, kernel {info['kernel']}")
    print(f"  cpu      {info['cpuModel']}, {info['cpuCores']} cores, {info['architecture']}")
    print(f"  memory   {info['totalMemoryBytes'] / 2**30:.1f} GiB")

    print("adapters:")
    for a in client.call("getAdapters").json():
        print(f"  {a['name']:<20} {a['macLinux']}  {a['description']}")

    # The skipped cycles count since start. A count that climbs steadily means that the period is
    # too short for this host.
    loop = client.call("getGameLoop").json()
    print(
        f"game loop: period {loop['periodUs']} us, {loop['achievedHz']:.1f} of "
        f"{loop['targetHz']:.0f} Hz, {loop['skippedCycles']} skipped cycle(s), "
        f"SCHED_FIFO {loop['schedFifo']}, memory locked {loop['memLocked']}"
    )


if __name__ == "__main__":
    run(main)
