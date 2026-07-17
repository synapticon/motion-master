#!/usr/bin/env python3
"""Initialise the fieldbus driver (POST /api/init).

Reads the driver + adapter from config.toml and brings up the driver on a
freshly started, uninitialised Motion Master. init is one-shot: a second call
returns 409 until you reset, which this script reports rather than treating as
a hard failure.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common import load_config, make_client  # noqa: E402


def main():
    client = make_client()

    fieldbus = load_config()["fieldbus"]
    print(f"init: driver={fieldbus['driver']} adapter={fieldbus['adapter']}")

    r = client.call("init", body=fieldbus, check=False)
    if r.status_code == 200:
        print("driver initialised")
    elif r.status_code == 409:
        print("already initialised -- run reset first (this is fine)")
    else:
        sys.exit(f"init failed ({r.status_code}): {r.text}")


if __name__ == "__main__":
    main()
