"""Shared helpers for the Motion Master Python examples.

Loads config.toml and builds an MMClient for the HTTP API. The host is kept
portless in config so both the HTTP API and the monitoring WebSocket can be
derived from it.
"""

from __future__ import annotations

import sys
import tomllib
from pathlib import Path

from mm_client import MMClient

HERE = Path(__file__).resolve().parent


def load_config():
    """Load config.toml."""
    path = HERE / "config.toml"
    if not path.exists():
        sys.exit(f"config.toml not found -- copy the template first:\n    cp {HERE / 'config.example.toml'} {path}")
    with open(path, "rb") as f:
        return tomllib.load(f)


def http_url(cfg):
    """HTTPS base URL for the request/response API, e.g. https://host:61447."""
    return f"https://{cfg['host']}:{cfg['http_port']}"


def ws_url(cfg):
    """WSS URL for the monitoring WebSocket, e.g. wss://host:62281."""
    return f"wss://{cfg['host']}:{cfg['ws_port']}"


def make_client(cfg=None):
    """Build an MMClient wired to both the HTTP API and the monitoring WebSocket."""
    cfg = cfg or load_config()
    return MMClient(http_url=http_url(cfg), ws_url=ws_url(cfg), verify=cfg.get("verify", False))


# EtherCAT AL states, in the AL control register encoding the API uses.
INIT = 1
PRE_OP = 2
BOOT = 3
SAFE_OP = 4
OP = 8

STATE_NAMES = {0: "UNKNOWN", INIT: "INIT", PRE_OP: "PRE-OP", BOOT: "BOOT", SAFE_OP: "SAFE-OP", OP: "OP"}

# The states a device climbs through, from the bottom. BOOT is not on it, because BOOT is
# reachable only from INIT.
_LADDER = [INIT, PRE_OP, SAFE_OP, OP]


def _next_step(current, target):
    """Return the next state on the way from current to target.

    The server refuses an illegal transition. A device may go down any number of states in one
    step, but it goes up one state at a time, and it enters and leaves BOOT only through INIT.
    """
    if current == BOOT or target == BOOT:
        return INIT if current != INIT else BOOT
    if _LADDER.index(target) < _LADDER.index(current):
        return target
    return _LADDER[_LADDER.index(current) + 1]


def _satisfied(current, target, exact):
    """Return whether a device in current already meets target."""
    if current == target:
        return True
    if exact or BOOT in (current, target):
        return False
    return _LADDER.index(current) > _LADDER.index(target)


def ensure_state(client, state, positions=None, exact=False):
    """Bring the server and the devices to the state a script needs.

    Opens the fieldbus driver from config.toml when none is open, scans when the last scan is
    missing or wrong, and moves each device to `state`. A device already above `state` stays
    where it is, because SAFE-OP and OP keep the mailbox of PRE-OP. Pass exact=True when the
    script needs that one state. A target of BOOT is always exact.

    Exits the script when a device does not answer or does not reach a state.
    """
    fieldbus = load_config()["fieldbus"]
    r = client.call("init", body=fieldbus, check=False)
    if r.status_code == 200:
        print(f"opened the {fieldbus.get('driver', 'soem')} driver")
    elif r.status_code != 409:
        sys.exit(f"init failed ({r.status_code}): {r.text}")

    # Over SPoE, devicesOnWire is null, so `required` stays false before the first scan.
    # devicesScanned is 0 until a scan ran on either driver.
    scan = client.call("getScanStatus").json()
    if scan["required"] or scan["devicesScanned"] == 0:
        print("scanning the bus")
        found = client.call("scan").json()["slaves"]
        print(f"found {found} device(s)")
        if found == 0:
            sys.exit("no devices on the bus")

    query = {"positions": ",".join(map(str, positions))} if positions else None
    while True:
        devices = client.call("getDeviceStates", query=query).json()
        for d in devices:
            if d["reachability"] != "answering":
                sys.exit(f"device {d['slavePosition']} is {d['reachability']} -- check the cable and the power")

        # Devices on different states can need different steps, so one request goes to each group.
        steps = {}
        for d in devices:
            if not _satisfied(d["alState"], state, exact):
                steps.setdefault(_next_step(d["alState"], state), []).append(d["slavePosition"])
        if not steps:
            return

        for step, group in steps.items():
            print(f"device(s) {group} -> {STATE_NAMES[step]}")
            result = client.call("transitionToState", body={"state": step, "positions": group}).json()
            if not result["ok"]:
                for d in result["devices"]:
                    if not d["reached"]:
                        name = STATE_NAMES.get(d["alState"], d["alState"])
                        print(f"  device {d['slavePosition']}: {name}, alStatusCode=0x{d['alStatusCode']:04x}")
                sys.exit(f"not every device reached {STATE_NAMES[step]}")
