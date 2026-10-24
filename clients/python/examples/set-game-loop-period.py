#!/usr/bin/env python3
"""Change the cycle period of the real-time game loop (PUT /api/game-loop).

The change applies at once and lasts until the server stops. It does not change the config file,
so a restart goes back to the configured period. A skipped cycle count that climbs steadily after
the change means that the period is too short for this host.

The script needs no fieldbus driver, so it opens none.

    python set-game-loop-period.py --period 2000
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import MotionMasterClient  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--period", type=int, required=True, help="cycle period in microseconds")
    args = ap.parse_args()

    client = MotionMasterClient.from_config()
    before = client.call("getGameLoop").json()["periodUs"]
    client.call("setGameLoopPeriod", body={"periodUs": args.period})
    after = client.call("getGameLoop").json()["periodUs"]
    print(f"game loop period {before} us -> {after} us")


if __name__ == "__main__":
    main()
