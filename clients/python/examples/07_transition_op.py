#!/usr/bin/env python3
"""Bring all devices up to OP (POST /api/devices/state), climbing one step at a time.

EtherCAT only permits single-step climbs (PRE-OP -> SAFE-OP -> OP), so reaching
OP means walking the ladder rather than jumping straight to state 8. Each rung is
a valid transition from the one below (and a no-op if already there), so this runs
correctly whether the bus is currently in INIT or PRE-OP.

OP is the exchanging state: it's the prerequisite for live monitoring
(08_monitor), where parameters read `null` until their device is in SAFE-OP/OP.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common import make_client  # noqa: E402

# AL states to climb through, in order, up to OP.
LADDER = [("PRE-OP", 2), ("SAFE-OP", 4), ("OP", 8)]


def main():
    client = make_client()

    for name, state in LADDER:
        result = client.call("transitionToState", body={"state": state}).json()
        if result["ok"]:
            print(f"-> {name}: ok")
            continue
        print(f"-> {name}: FAILED")
        for d in result["devices"]:
            if not d["reached"]:
                print(f"     slave {d['slavePosition']}: alStatusCode=0x{d['alStatusCode']:04x}")
        sys.exit(1)

    print("all devices in OP")


if __name__ == "__main__":
    main()
