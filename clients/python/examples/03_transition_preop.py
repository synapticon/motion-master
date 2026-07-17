#!/usr/bin/env python3
"""Transition all devices to PRE-OP (POST /api/devices/state).

AL state 2 = PreOp. Omitting `positions` targets every discovered device. The
response reports the settled state of each device with a `reached` flag; `ok` is
true only when they all arrived.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common import make_client  # noqa: E402

PREOP = 2


def main():
    client = make_client()
    result = client.call("transitionToState", body={"state": PREOP}).json()

    for d in result["devices"]:
        mark = "ok" if d["reached"] else f"FAILED (alStatusCode=0x{d['alStatusCode']:04x})"
        print(f"  slave {d['slavePosition']}: alState={d['alState']} {mark}")

    print("all devices reached PRE-OP" if result["ok"] else "some devices did not reach PRE-OP")
    sys.exit(0 if result["ok"] else 1)


if __name__ == "__main__":
    main()
