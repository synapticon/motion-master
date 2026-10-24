"""A reference Motion Master HTTP API client driven by the OpenAPI spec.

On construction the client fetches the running server's own OpenAPI spec from
GET /api/swagger.yml, indexes every operation by its operationId, and resolves
the HTTP method + URL path template from that spec at call time. Example scripts
therefore never hardcode a URL or verb -- they name the operationId straight out
of the spec the server itself is serving:

    client.call("init", body={"driver": "soem", "adapter": "eth0"})
    client.call("sdoUpload", path={"slavePosition": 1, "index": 0x6041, "subindex": 0})

Reading the spec from the live server (rather than a file in this repo) means
the client is standalone -- no source checkout needed -- and always matches the
exact contract of the server it is talking to. The spec is parsed with PyYAML:
we only need operationId -> method + path, so a generic YAML load is enough (and
avoids a strict OpenAPI validator rejecting perfectly legal media types such as
the text/yaml this very endpoint declares).

On top of call(), the client has the steps that most scripts begin with. init()
opens the fieldbus driver and accepts one that is open already. ensure_scanned()
scans when needed and moves no device. ensure_state() scans when needed and
moves the devices to an AL state. from_config() builds a client from
config.toml. The steps log through the "motion_master_client" logger, so a
script that wants to see them turns on logging at INFO. run_procedure() starts a
procedure, polls it until it ends, and cancels it when the caller stops early.
monitor() and notifications() stream from the WebSocket.
"""

from __future__ import annotations

import json
import logging
import ssl
import time
import tomllib
from pathlib import Path

import requests
import urllib3
import yaml

# OpenAPI path-item keys that denote an operation (as opposed to parameters,
# summary, etc.); each maps operationId -> (method, path template).
_HTTP_METHODS = {"get", "put", "post", "delete", "patch", "head", "options", "trace"}

log = logging.getLogger("motion_master_client")

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


class StateError(Exception):
    """A device does not answer, or does not reach the AL state it was commanded to."""


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


class MotionMasterClient:
    """Thin wrapper over requests that resolves operations from the server's OpenAPI spec."""

    def __init__(self, http_url, ws_url=None, verify=False, fieldbus=None):
        self._http_url = http_url.rstrip("/")
        # The body of POST /api/init, for example {"driver": "soem", "adapter": "eth0"}.
        self.fieldbus = fieldbus
        # The monitoring WebSocket endpoint. Held here so the client is the one
        # connection object -- a future monitor()/trace-process-data method reads
        # this instead of callers wiring up a second object.
        self.ws_url = ws_url
        self._verify = verify
        self._session = requests.Session()

        # A self-signed dev cert (verify=False) would otherwise spam a warning
        # on every request and drown the example output.
        if verify is False:
            urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

        # Fetch and parse the server's own spec: operationId -> (http method,
        # path template such as "/api/devices/{slavePosition}/sdo/{index}/{subindex}").
        response = self._session.get(f"{self._http_url}/api/swagger.yml", verify=verify)
        response.raise_for_status()
        spec = yaml.safe_load(response.text)

        self._ops = {}
        for template, item in spec.get("paths", {}).items():
            for method, op in item.items():
                if method in _HTTP_METHODS and isinstance(op, dict) and "operationId" in op:
                    self._ops[op["operationId"]] = (method.upper(), template)

    def call(self, operation_id, path=None, body=None, query=None, check=True, **kwargs):
        """Invoke an operation by its operationId.

        path/body/query map to URL path params, the JSON request body, and the
        query string. Extra kwargs are forwarded to requests (e.g. data=... for
        the raw octet-stream FoE upload). Set check=False to inspect non-2xx
        responses yourself (e.g. init's 409 "already initialised").
        """
        if operation_id not in self._ops:
            raise KeyError(f"unknown operationId {operation_id!r} (not in the OpenAPI spec)")

        method, template = self._ops[operation_id]
        url = self._http_url + template.format(**(path or {}))

        response = self._session.request(
            method, url, json=body, params=query, verify=self._verify, **kwargs
        )
        if check:
            response.raise_for_status()
        return response

    @classmethod
    def from_config(cls, path=None):
        """Build a client from a TOML config file, by default config.toml next to this module.

        The file gives the host, the two ports, the TLS verification and the [fieldbus] table
        that init() sends. config.example.toml is the template.
        """
        path = Path(path) if path else Path(__file__).resolve().parent / "config.toml"
        if not path.exists():
            raise FileNotFoundError(
                f"{path} not found -- copy the template first:\n"
                f"    cp {path.parent / 'config.example.toml'} {path}"
            )
        with open(path, "rb") as f:
            cfg = tomllib.load(f)
        return cls(
            http_url=f"https://{cfg['host']}:{cfg['http_port']}",
            ws_url=f"wss://{cfg['host']}:{cfg['ws_port']}",
            verify=cfg.get("verify", False),
            fieldbus=cfg.get("fieldbus"),
        )

    def init(self, fieldbus=None):
        """Open the fieldbus driver, unless one is open already.

        Uses `fieldbus` when given, otherwise the settings passed to the constructor. Unlike
        call("init", ...), a 409 "already initialised" counts as success, so a script can call
        this every time it starts.
        """
        body = fieldbus or self.fieldbus
        if body is None:
            raise ValueError("no fieldbus settings: pass them to MotionMasterClient or to init()")
        r = self.call("init", body=body, check=False)
        if r.status_code == 200:
            log.info("opened the %s driver", body.get("driver", "soem"))
        elif r.status_code != 409:
            r.raise_for_status()

    def ensure_scanned(self):
        """Scan when the last scan is missing or wrong, and move no device.

        A script that needs the devices known but no AL state calls this instead of ensure_state(),
        so that it leaves every device where it is, BOOT included. Call init() first.

        Raises StateError when the scan finds no device.
        """
        # Over SPoE, devicesOnWire is null, so `required` stays false before the first scan.
        # devicesScanned is 0 until a scan ran on either driver.
        scan = self.call("getScanStatus").json()
        if scan["required"] or scan["devicesScanned"] == 0:
            log.info("scanning the bus")
            found = self.call("scan").json()["slaves"]
            log.info("found %d device(s)", found)
            if found == 0:
                raise StateError("no devices on the bus")

    def ensure_state(self, state, positions=None, exact=False):
        """Bring the devices to the AL state a script needs.

        Scans when the last scan is missing or wrong, then moves each device to `state` one legal
        step at a time. A device already above `state` stays where it is, because SAFE-OP and OP
        keep the mailbox of PRE-OP. Pass exact=True when the script needs that one state. A target
        of BOOT is always exact. Call init() first.

        Raises StateError when a device does not answer or does not reach a step.
        """
        self.ensure_scanned()

        query = {"positions": ",".join(map(str, positions))} if positions else None
        while True:
            devices = self.call("getDeviceStates", query=query).json()
            for d in devices:
                if d["reachability"] != "answering":
                    raise StateError(f"device {d['slavePosition']} is {d['reachability']}")

            # Devices on different states can need different steps, so one request goes to each
            # group.
            steps = {}
            for d in devices:
                if not _satisfied(d["alState"], state, exact):
                    steps.setdefault(_next_step(d["alState"], state), []).append(d["slavePosition"])
            if not steps:
                return

            for step, group in steps.items():
                log.info("device(s) %s -> %s", group, STATE_NAMES[step])
                body = {"state": step, "positions": group}
                result = self.call("transitionToState", body=body).json()
                if not result["ok"]:
                    failed = ", ".join(
                        f"device {d['slavePosition']} in {STATE_NAMES.get(d['alState'], d['alState'])}"
                        f" with alStatusCode 0x{d['alStatusCode']:04x}"
                        for d in result["devices"]
                        if not d["reached"]
                    )
                    raise StateError(f"not every device reached {STATE_NAMES[step]}: {failed}")

    def run_procedure(self, position, name, params=None, poll_interval=0.5):
        """Start a procedure on a device and yield its snapshot at every poll until the run ends.

        The last snapshot yielded has the final status: succeeded, failed or cancelled. Polling is
        lossless, because every finished step keeps its status and value, so a step that starts
        and ends between two polls still shows. When the caller stops iterating before the run
        ends, for example on Ctrl-C, the run is cancelled, so that the drive does not continue
        alone.

        Raises requests.HTTPError when the server refuses the start, for example with 409 when
        another procedure runs on the device.
        """
        path = {"slavePosition": position, "procedureName": name}
        snapshot = self.call("startProcedure", path=path, body=params or {}).json()
        try:
            while True:
                yield snapshot
                if snapshot["status"] != "running":
                    return
                time.sleep(poll_interval)
                snapshot = self.call("getProcedure", path=path).json()
        finally:
            if snapshot["status"] == "running":
                log.info("cancelling %s on device %d", name, position)
                self.call("cancelProcedure", path=path, check=False)

    def notifications(self):
        """Subscribe to the notifications topic and yield the data of each notification.

        Every notification source publishes to the one "notifications" topic, so one subscription
        receives every event, also a kind that a later server adds. data["event"] names the kind.
        The caller stops by breaking out of the loop.
        """
        for msg in self._subscribe("notifications"):
            if msg.get("type") == "notification":
                yield msg["data"]

    def _subscribe(self, topic):
        """Open the WebSocket, subscribe to a topic, and yield every message as parsed JSON."""
        if not self.ws_url:
            raise RuntimeError("no ws_url configured on this client")

        # Lazy import so HTTP-only usage doesn't require the websockets package.
        from websockets.sync.client import connect

        with connect(self.ws_url, ssl=self._ssl_context()) as ws:
            ws.send(json.dumps({"subscribe": topic}))
            try:
                for message in ws:
                    yield json.loads(message)
            finally:
                # Best-effort unsubscribe; the socket close below also ends it.
                try:
                    ws.send(json.dumps({"unsubscribe": topic}))
                except Exception:
                    pass

    def _ssl_context(self):
        """Build an SSL context matching the HTTP verify setting for the WebSocket."""
        if self._verify is False:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            return ctx
        if isinstance(self._verify, str):
            return ssl.create_default_context(cafile=self._verify)
        return ssl.create_default_context()

    def monitor(self, topic):
        """Subscribe to a monitoring topic and yield its batches from the WebSocket.

        The monitoring itself must already exist (POST /api/monitorings, i.e.
        client.call("createMonitoring", ...)). This opens the WebSocket, sends
        {"subscribe": topic}, and yields each batch's rows -- a list of
        [timestampUs, v0, v1, ...] cycle rows, positionally ordered by the
        monitoring's parameters. The stream is lossless: every recorded cycle
        since the last flush arrives in the batch. The caller stops by breaking
        out of the loop; the socket and subscription are torn down on exit.
        """
        for msg in self._subscribe(topic):
            if msg.get("type") == "monitoring" and msg.get("topic") == topic:
                yield msg["data"]
