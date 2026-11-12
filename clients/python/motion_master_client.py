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
step_changes() picks out the steps whose status changed, and format_step() turns
one into a line.
monitor() and notifications() stream from the WebSocket. monitoring() creates a
monitoring, records its rows while a with block runs, and deletes it at the
end, so a script can check the rows afterwards. read_value() reads one object,
decoded by the server, and wait_for_value() reads it until a condition holds.
Profile, Cia402 and Somanet name every object of a SOMANET device as an address,
such as Cia402.STATUSWORD, generated in motion_master_objects.py. AlState names the
AL states, Cia402State the CiA402 states, and Cia402.state() names the state a
statusword reports.
devices() lists the scanned devices, and parameters() and hardware_description()
read one device. restart() restarts devices through INIT up to an AL state, and
set_operation_mode(), set_target(), enable(), disable() and quick_stop() drive
one CiA402 drive.
run(main) calls a script's main() and turns an error a user can act on, such as
a server that is not running, into one line on stderr.
"""

from __future__ import annotations

import csv
import enum
import functools
import json
import logging
import os
import ssl
import sys
import threading
import time
import tomllib
import urllib.parse
import uuid
from contextlib import ExitStack
from pathlib import Path
from typing import Callable, TypeVar

import requests
import urllib3
import yaml

# The generated addresses. A script imports them from here, with the rest of the client.
from motion_master_objects import Cia402 as _Cia402Objects
from motion_master_objects import ObjectAddress, Profile, Somanet  # noqa: F401

_T = TypeVar("_T")

# OpenAPI path-item keys that denote an operation (as opposed to parameters,
# summary, etc.); each maps operationId -> (method, path template).
_HTTP_METHODS = {"get", "put", "post", "delete", "patch", "head", "options", "trace"}

log = logging.getLogger("motion_master_client")

class AlState(enum.IntEnum):
    """An AL state, in the encoding of the AL control register that the API uses.

    It is an int, so a member goes into a request body as it is, and AlState(value) converts an
    alState from a response. The name is the label, such as PREOP. The server reports UNKNOWN for a
    device that does not answer.
    """

    UNKNOWN = 0
    INIT = 1
    PREOP = 2
    BOOT = 3
    SAFEOP = 4
    OP = 8


class Cia402State(enum.StrEnum):
    """A CiA402 state, by the name the API uses. A member equals its name: OPERATION_ENABLED ==
    "OperationEnabled"."""

    NOT_READY_TO_SWITCH_ON = "NotReadyToSwitchOn"
    SWITCH_ON_DISABLED = "SwitchOnDisabled"
    READY_TO_SWITCH_ON = "ReadyToSwitchOn"
    SWITCHED_ON = "SwitchedOn"
    OPERATION_ENABLED = "OperationEnabled"
    QUICK_STOP_ACTIVE = "QuickStopActive"
    FAULT_REACTION_ACTIVE = "FaultReactionActive"
    FAULT = "Fault"


class Cia402(_Cia402Objects):
    """The CiA402 objects (0x6xxx) as addresses, and the decode of a statusword."""

    @staticmethod
    def state(statusword: int) -> Cia402State:
        """Return the CiA402 state that a statusword (0x6041) reports.

        The same masks as cia402::decodeState on the server, so a state decoded here from a
        recorded row has the name that GET /api/devices/{slavePosition}/cia402 gives. A pattern
        that matches no state is NOT_READY_TO_SWITCH_ON.
        """
        if statusword & 0x4F == 0x40:
            return Cia402State.SWITCH_ON_DISABLED
        if statusword & 0x6F == 0x21:
            return Cia402State.READY_TO_SWITCH_ON
        if statusword & 0x6F == 0x23:
            return Cia402State.SWITCHED_ON
        if statusword & 0x6F == 0x27:
            return Cia402State.OPERATION_ENABLED
        if statusword & 0x6F == 0x07:
            return Cia402State.QUICK_STOP_ACTIVE
        if statusword & 0x4F == 0x0F:
            return Cia402State.FAULT_REACTION_ACTIVE
        if statusword & 0x4F == 0x08:
            return Cia402State.FAULT
        return Cia402State.NOT_READY_TO_SWITCH_ON


# The states a device climbs through, from the bottom. BOOT is not on it, because BOOT is
# reachable only from INIT. _satisfied() orders the states with it.
_LADDER = [AlState.INIT, AlState.PREOP, AlState.SAFEOP, AlState.OP]


def _path(position, **more):
    """Return the path parameters that address the device at `position`, plus any others."""
    return {"slavePosition": position, **more}


def _not_reached(result, step):
    """Say which devices of a state change did not reach `step`, and why."""
    failed = ", ".join(
        f"device {d['slavePosition']} in {AlState(d['alState']).name}"
        f" with alStatusCode 0x{d['alStatusCode']:04x}"
        for d in result["devices"]
        if not d["reached"]
    )
    return f"not every device reached {AlState(step).name}: {failed}"


class StateError(Exception):
    """A device does not answer, or does not reach the AL state it was commanded to."""


class ProcedureError(Exception):
    """The server refuses to start a procedure. The message is the server's reason."""


class CaptureError(Exception):
    """The server refuses to start or stop a capture. The message is the server's reason."""


class ProcessDataDumpError(Exception):
    """The server could not produce a process-data dump. The message is the server's reason."""


# A flag bit of a .mmpd file, at byte 6: the dump is of a span, and the server no longer held every
# cycle right after its start. A re-map, a reset or a scan discarded some, or the ring overwrote
# them. The rows are all in the span, but the first one is later than the span asked for.
MMPD_FLAG_INCOMPLETE_START = 0x0001


def mmpd_flags(data):
    """Return the flags of a .mmpd file, the MMPD_FLAG_* bits."""
    return int.from_bytes(data[6:8], "little")


def _reason(response):
    """Return the server's reason for a refused request: the error of its JSON body, or its text."""
    try:
        return response.json()["error"]
    except (ValueError, KeyError, TypeError):
        return response.text.strip()


def _satisfied(current, target, exact):
    """Return whether a device in current already meets target."""
    if current == target:
        return True
    if exact or AlState.BOOT in (current, target):
        return False
    return _LADDER.index(current) > _LADDER.index(target)


def step_changes(run):
    """Yield each snapshot of a run from run_procedure(), with the steps whose status changed.

    Every snapshot is yielded, as (snapshot, changed). `changed` lists the steps whose status is not
    the one they had in the snapshot before, and it can be empty. A step not reached yet is idle,
    so it is listed only once it starts. The last snapshot carries the final status of the run.
    Closing this generator closes `run`, which cancels a run that is still in progress.
    """
    # A step that starts and ends between two polls shows only once, already finished, so a step
    # is compared with its last status, not with the poll before.
    seen = {}
    try:
        for snapshot in run:
            changed = []
            for step in snapshot["steps"]:
                if seen.get(step["id"], "idle") != step["status"]:
                    seen[step["id"]] = step["status"]
                    changed.append(step)
            yield snapshot, changed
    finally:
        run.close()


def format_step(step):
    """Return one line for a step of a procedure: its id, its status, and its value or error."""
    line = f"  {step['id']:<36} {step['status']}"
    if "value" in step:
        line += f"  {json.dumps(step['value'], ensure_ascii=False)}"
    if "error" in step:
        line += f"  {step['error']}"
    return line


def error_line(error):
    """Return one line that names an error a user can act on, or None for any other error."""
    if isinstance(error, requests.ConnectionError):
        url = urllib.parse.urlsplit(error.request.url) if error.request else None
        where = f"{url.scheme}://{url.netloc}" if url else "the server"
        return f"cannot connect to {where}. Is the server running, and is config.toml correct?"
    if isinstance(error, requests.HTTPError):
        r = error.response
        try:
            reason = r.json()["error"]
        except (ValueError, KeyError, TypeError):
            reason = r.text or r.reason
        path = urllib.parse.urlsplit(r.url).path
        return f"{r.request.method} {path}: HTTP {r.status_code}, {reason}"
    if isinstance(error, (StateError, ProcedureError, FileNotFoundError, ConnectionError)):
        return str(error)
    return None


def run(main):
    """Call main() and exit with one line on stderr when it fails with an error a user can act on.

    Those errors are: no connection to the server, an HTTP error status, a StateError, a
    ProcedureError and a missing file. Any other error is a bug, so it keeps its traceback. Set
    MM_TRACEBACK=1 to get the traceback for every error.
    """
    try:
        main()
    except Exception as error:
        line = error_line(error)
        if line is None or os.environ.get("MM_TRACEBACK") == "1":
            raise
        print(f"error: {line}", file=sys.stderr)
        sys.exit(1)


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
        # A copy beside this module lets a reader, or a coding assistant that writes a new script,
        # look up an operationId without a running server.
        (Path(__file__).resolve().parent / "swagger.yml").write_text(response.text, encoding="utf-8")
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

    def reset(self):
        """Close the fieldbus driver and forget every device (reset).

        The server commands no AL state, so the devices stay in the state they are in. A reset with
        no driver open also succeeds. Call init() before the next step that needs the bus.
        """
        self.call("reset")
        log.info("closed the driver and forgot every device")

    def scan(self):
        """Scan the bus and return the number of devices found (scan).

        The devices stay in INIT. Call init() first. Unlike ensure_scanned(), this scans every time
        and returns 0 rather than raising when the bus is empty.
        """
        found = self.call("scan").json()["slaves"]
        log.info("found %d device(s)", found)
        return found

    def start_capture(self, adapter=None, seconds=None):
        """Start a capture of the EtherCAT frames that runs until stop_capture() (startCapture).

        Returns once the capture records, with the path of its pcap file in the user cache. The
        capture runs as a program of its own, so a reset, an init or a scan in between does not end
        it. `adapter` defaults to the adapter of the bus, so name it when no bus is up. `seconds`
        is the upper limit for a capture that is never stopped, 600 when not given.

        Raises CaptureError with the server's reason when it refuses, for example while another
        capture runs (409) or on Windows (501).
        """
        body = {}
        if adapter:
            body["adapter"] = adapter
        if seconds is not None:
            body["seconds"] = seconds
        r = self.call("startCapture", body=body, check=False)
        if not r.ok:
            raise CaptureError(f"the capture did not start: {_reason(r)} (HTTP {r.status_code})")
        started = r.json()
        log.info("capturing on %s into %s", started["adapter"], started["path"])
        return started["path"]

    def stop_capture(self):
        """Stop the started capture and return its path and summary (stopCapture).

        The result is the body of POST /api/captures/stop: `path`, the pcap file in the user cache,
        and `summary`. Raises CaptureError with the server's reason when no capture was started or
        the capture failed.
        """
        r = self.call("stopCapture", check=False)
        if not r.ok:
            raise CaptureError(
                f"the capture did not stop cleanly: {_reason(r)} (HTTP {r.status_code})"
            )
        stopped = r.json()
        log.info("stopped the capture into %s", stopped["path"])
        return stopped

    def process_data_mark(self):
        """Return the recorder clock of the server now, in epoch microseconds (getProcessDataMark).

        Pass it as `from_us` or `to_us` of process_data_dump() to dump one part of a run. It is the
        server's clock, the one that stamps every recorded cycle, so it matches the cycles also
        when this client runs on another computer.
        """
        return self.call("getProcessDataMark").json()["timestampUs"]

    def process_data_dump(self, from_us=None, to_us=None):
        """Return the .mmpd dump of the recorder ring as bytes (streamProcessDataDump).

        `from_us` and `to_us` limit it to the cycles of a span, both included, in epoch
        microseconds of the server clock: take them from process_data_mark(). Without them the
        dump is the whole ring. When the server no longer held every cycle right after `from_us`,
        mmpd_flags() of the result has MMPD_FLAG_INCOMPLETE_START set.

        Returns None when the server has no cycle to dump: no process image was ever mapped, or no
        cycle is in the span (409). Raises ProcessDataDumpError for every other refusal.
        """
        query = {}
        if from_us is not None:
            query["from"] = from_us
        if to_us is not None:
            query["to"] = to_us
        r = self.call("streamProcessDataDump", query=query, check=False)
        if r.status_code == 409:
            log.info("no process-data dump: %s", _reason(r))
            return None
        if not r.ok:
            raise ProcessDataDumpError(
                f"the process-data dump failed: {_reason(r)} (HTTP {r.status_code})"
            )
        return r.content

    def devices(self):
        """Return the scanned devices, in bus order, as GET /api/devices gives them (getDevices).

        Each is a dict with slavePosition, productName, productCode and the other fields of that
        response. The list is empty before the first scan.
        """
        return self.call("getDevices").json()

    def parameters(self, position):
        """Return every parameter of a device, each with its value and metadata.

        The list is the object dictionary as the server enumerated it (getDeviceParameters).
        """
        return self.call("getDeviceParameters", path=_path(position)).json()

    def hardware_description(self, position):
        """Return the .hardware_description of a device, parsed (getDeviceHardwareDescription)."""
        return self.call("getDeviceHardwareDescription", path=_path(position)).json()

    def read_value(self, position: int, obj: ObjectAddress[_T]) -> _T:
        """Read one object of a device and return its value, decoded by the server.

        `obj` is an address, for example Cia402.STATUSWORD, or (index, subindex). The server takes
        the value from the process data when the device exchanges it, and reads it over SDO
        otherwise. It decodes the value with the data type from the object dictionary, so the
        caller needs no type. With an address, a type checker knows the type of the value.
        """
        index, subindex = obj
        path = _path(position, index=index, subindex=subindex)
        return self.call("readParameter", path=path).json()["value"]

    def wait_for_value(
        self,
        position: int,
        obj: ObjectAddress[_T],
        predicate: Callable[[_T], object],
        timeout: float = 1.0,
        interval: float = 0.05,
    ) -> _T:
        """Read an object until `predicate(value)` is true, and return that value.

        `obj` is an address, for example Cia402.STATUSWORD, or (index, subindex). Each read is
        read_value(), so a PDO-mapped object of an exchanging device costs no bus transfer.

            client.wait_for_value(1, Cia402.STATUSWORD, lambda sw: sw & 0x0400)  # target reached

        Raises TimeoutError with the last value when the predicate is still false after `timeout`
        seconds.
        """
        deadline = time.monotonic() + timeout
        while True:
            value = self.read_value(position, obj)
            if predicate(value):
                return value
            if time.monotonic() >= deadline:
                index, subindex = obj
                raise TimeoutError(
                    f"device {position} 0x{index:04x}:{subindex:02x} is {value} after {timeout} s"
                )
            time.sleep(interval)

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
            if self.scan() == 0:
                raise StateError("no devices on the bus")

    def ensure_state(self, state, positions=None, exact=False):
        """Bring the devices to the AL state a script needs.

        Scans when the last scan is missing or wrong, then asks the server to walk each device to
        `state` (transitionToState), one legal transition at a time. A device already above
        `state` stays where it is, because SAFEOP and OP keep the mailbox of PREOP. Pass
        exact=True when the script needs that one state. A target of BOOT is always exact. Call
        init() first.

        Raises StateError when a device does not answer or does not reach `state`.
        """
        self.ensure_scanned()

        query = {"positions": ",".join(map(str, positions))} if positions else None
        devices = self.call("getDeviceStates", query=query).json()
        for d in devices:
            if d["reachability"] != "answering":
                raise StateError(f"device {d['slavePosition']} is {d['reachability']}")
        group = [d["slavePosition"] for d in devices if not _satisfied(d["alState"], state, exact)]
        if not group:
            return
        log.info("device(s) %s -> %s", group, AlState(state).name)
        result = self.call("transitionToState", body={"state": state, "positions": group}).json()
        if not result["ok"]:
            raise StateError(_not_reached(result, state))

    def restart(self, target, positions=None):
        """Restart devices through INIT, then walk them up to the AL state `target`.

        The server does the walk (restartDevices), one state at a time. Without `positions`, every
        scanned device restarts. Scans first when the last scan is missing or wrong. Call init()
        first. A restart drops each device out of process data exchange, so a drive that was
        enabled is disabled.

        Raises StateError when a device does not reach a step.
        """
        self.ensure_scanned()
        body = {"target": target}
        if positions:
            body["positions"] = list(positions)
        log.info("restart device(s) %s -> %s", positions or "all", AlState(target).name)
        result = self.call("restartDevices", body=body).json()
        if not result["ok"]:
            raise StateError(_not_reached(result, result["step"]))

    def set_operation_mode(self, position, mode):
        """Write the CiA402 operation mode (0x6060) of a drive (setCia402OperationMode).

        The server waits until 0x6061 shows the mode, so the drive runs in it when this returns.
        Returns the CiA402 snapshot. Raises requests.HTTPError, 409, when the drive does not take
        the mode.
        """
        body = {"mode": mode}
        return self.call("setCia402OperationMode", path=_path(position), body=body).json()

    def set_target(self, position, kind, value):
        """Write the target of one kind, "position", "velocity" or "torque" (setCia402Target).

        The value is in the units of the object: 0x607A, 0x60FF or 0x6071.
        """
        body = {"target": kind, "value": value}
        self.call("setCia402Target", path=_path(position), body=body)

    def enable(self, position):
        """Bring a drive to Operation Enabled, and return its CiA402 snapshot.

        The server walks the CiA402 state machine (transitionToCia402State), and it leaves a quick
        stop. The device must exchange process data.
        """
        return self._to_cia402_state(position, Cia402State.OPERATION_ENABLED)

    def disable(self, position):
        """Bring a drive to Switch On Disabled, and return its CiA402 snapshot."""
        return self._to_cia402_state(position, Cia402State.SWITCH_ON_DISABLED)

    def quick_stop(self, position):
        """Send a quick stop to a drive (runCia402Command), and return its CiA402 snapshot.

        It returns at once. The drive stops on its quick stop ramp, and 0x605A says the state it
        ends in.
        """
        body = {"command": "quickStop"}
        return self.call("runCia402Command", path=_path(position), body=body).json()

    def _to_cia402_state(self, position, state):
        body = {"state": state}
        return self.call("transitionToCia402State", path=_path(position), body=body).json()

    def run_procedure(self, position, name, params=None, poll_interval=0.5):
        """Start a procedure on a device and yield its snapshot at every poll until the run ends.

        The last snapshot yielded has the final status: succeeded, failed or cancelled. Polling is
        lossless, because every finished step keeps its status and value, so a step that starts
        and ends between two polls still shows. When the caller stops iterating before the run
        ends, for example on Ctrl-C, the run is cancelled, so that the drive does not continue
        alone.

        Raises ProcedureError when the server refuses the start: 409 when another procedure runs
        on the device, 404 when the device has no procedure of that name, 400 for a parameter the
        procedure refuses.
        """
        path = _path(position, procedureName=name)
        r = self.call("startProcedure", path=path, body=params or {}, check=False)
        if not r.ok:
            try:
                reason = r.json()["error"]
            except (ValueError, KeyError):
                reason = r.text
            raise ProcedureError(f"{name} on device {position}: {reason} (HTTP {r.status_code})")
        snapshot = r.json()
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

    def monitoring(self, parameters, interval_ms=50, topic=None, name=None):
        """Return a Monitoring that records the rows of the parameters while a with block runs.

        `parameters` is a list of (devicePosition, index, subindex). Create it after the devices
        are in SAFEOP or OP, because the server decides once, at creation, whether each object
        comes from the process data or from SDO reads. Without `topic`, a unique one is chosen.

            with client.monitoring([(1, *Cia402.VELOCITY_ACTUAL_VALUE)]) as m:
                ...  # the move
            velocity = m.column(1, Cia402.VELOCITY_ACTUAL_VALUE)
        """
        topic = topic or f"py-{uuid.uuid4().hex[:12]}"
        return Monitoring(self, parameters, interval_ms, topic, name)


@functools.cache
def _object_names():
    """Map (index, subindex) to the generated name of every object, for example STATUSWORD."""
    names = {}
    for group in (Profile, _Cia402Objects, Somanet):
        for name, value in vars(group).items():
            if isinstance(value, ObjectAddress):
                names.setdefault(tuple(value), name)
    return names


def _object_name(index, subindex):
    """Return the generated name of an object, or 0xIIII:SS when it has none."""
    return _object_names().get((index, subindex), f"0x{index:04X}:{subindex:02X}")


class Monitoring:
    """A monitoring on the server, and every row it sent while the with block ran.

    Entering the block creates the monitoring, subscribes to its topic, and returns when the first
    row arrives, so nothing the block does is missed. Leaving the block waits for the rows of the
    last flush, closes the WebSocket and deletes the monitoring, also when the block raised. The
    rows stay readable after the block. An error of the thread that reads the WebSocket is raised
    when the block ends.
    """

    # How long entering waits for the first row, and leaving for the last flush.
    _FIRST_ROW_TIMEOUT = 5.0
    _LAST_FLUSH_TIMEOUT = 2.0

    def __init__(self, client, parameters, interval_ms, topic, name):
        self._client = client
        self._request = [[p, i, s] for p, i, s in parameters]
        self._interval_ms = interval_ms
        self.topic = topic
        self._name = name
        # The parameters as the server answered them, each with its source: "pdo" or "sdo".
        self.parameters = []
        # Every row, [timestampUs, v0, v1, ...] in the order of self.parameters.
        self.rows = []
        self._lock = threading.Lock()
        self._batches = 0
        self._arrived = threading.Condition(self._lock)
        # Holds the WebSocket connection, entered as a context manager, until _stop() closes it.
        self._connection = ExitStack()
        self._ws = None
        self._thread = None
        self._error = None

    @property
    def sources(self):
        """The source of each parameter, in order: "pdo" or "sdo"."""
        return [p["source"] for p in self.parameters]

    def column(self, position, obj):
        """Return [(timestampUs, value), ...] of one object, in the order the cycles ran.

        `obj` is an address, for example Cia402.STATUSWORD, or (index, subindex).
        """
        index, subindex = obj
        for i, p in enumerate(self.parameters):
            if (p["devicePosition"], p["index"], p["subindex"]) == (position, index, subindex):
                with self._lock:
                    return [(row[0], row[i + 1]) for row in self.rows]
        raise KeyError(f"device {position} 0x{index:04x}:{subindex:02x} is not in this monitoring")

    def values(self, position, obj, start=None, end=None):
        """Return the values of one object from the cycles with start <= timestampUs < end.

        Without `start` or `end`, the window is open on that side, so values(position, obj) gives
        every value.
        """
        return [
            v
            for t, v in self.column(position, obj)
            if (start is None or t >= start) and (end is None or t < end)
        ]

    def first(self, position, obj, predicate, after=None):
        """Return the timestampUs of the first cycle where predicate(value) is true, or None.

        With `after`, only the cycles with timestampUs >= after count.
        """
        for t, v in self.column(position, obj):
            if (after is None or t >= after) and predicate(v):
                return t
        return None

    def write_csv(self, path):
        """Write every row to a CSV file at `path`, and create its folder when it is missing.

        The columns are time_s, the seconds from the first row; timestamp_us, the time on the
        server when the cycle ran, in epoch microseconds; then one column per parameter, in order.
        A parameter's column is named after its device position and its object, 1:STATUSWORD, or
        1:0x2110:03 for an object with no generated name. A value is empty in a cycle where its
        device did not exchange process data.
        """
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        with self._lock:
            rows = list(self.rows)
        start = rows[0][0] if rows else 0
        header = ["time_s", "timestamp_us"]
        header += [
            f"{p['devicePosition']}:{_object_name(p['index'], p['subindex'])}"
            for p in self.parameters
        ]
        with path.open("w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(header)
            for timestamp_us, *values in rows:
                times = [f"{(timestamp_us - start) / 1e6:.6f}", timestamp_us]
                writer.writerow(times + ["" if v is None else v for v in values])

    def __enter__(self):
        body = {"topic": self.topic, "interval": self._interval_ms, "parameters": self._request}
        if self._name is not None:
            body["name"] = self._name
        r = self._client.call("createMonitoring", body=body, check=False)
        if r.status_code != 201:
            try:
                reason = r.json()["error"]
            except (ValueError, KeyError):
                reason = r.text
            raise RuntimeError(
                f"cannot create monitoring {self.topic}: {reason} (HTTP {r.status_code})"
            )
        self.parameters = r.json()["parameters"]
        try:
            self._start()
        except BaseException:
            self._stop()
            raise
        return self

    def __exit__(self, exc_type, exc, tb):
        try:
            if self._thread is not None and self._thread.is_alive():
                # A row is in a batch only after the next flush, so wait for two more batches.
                with self._lock:
                    target = self._batches + 2
                    self._arrived.wait_for(
                        lambda: self._batches >= target or self._error is not None,
                        timeout=self._LAST_FLUSH_TIMEOUT,
                    )
        finally:
            self._stop()
        if self._error is not None and exc_type is None:
            raise RuntimeError(f"monitoring {self.topic}: {self._error!r}") from self._error
        return False

    def _start(self):
        if not self._client.ws_url:
            raise RuntimeError("no ws_url configured on this client")
        # Lazy import so HTTP-only usage doesn't require the websockets package.
        from websockets.sync.client import connect

        self._ws = self._connection.enter_context(
            connect(self._client.ws_url, ssl=self._client._ssl_context())
        )
        self._ws.send(json.dumps({"subscribe": self.topic}))
        self._thread = threading.Thread(
            target=self._read, name=f"monitoring-{self.topic}", daemon=True
        )
        self._thread.start()
        with self._lock:
            arrived = self._arrived.wait_for(
                lambda: self.rows or self._error is not None, timeout=self._FIRST_ROW_TIMEOUT
            )
        if self._error is not None:
            raise RuntimeError(f"monitoring {self.topic}: {self._error!r}") from self._error
        if not arrived:
            raise TimeoutError(f"monitoring {self.topic}: no row after {self._FIRST_ROW_TIMEOUT} s")

    def _read(self):
        from websockets.exceptions import ConnectionClosedOK

        try:
            for message in self._ws:
                msg = json.loads(message)
                if msg.get("type") == "monitoring" and msg.get("topic") == self.topic:
                    with self._lock:
                        self.rows.extend(msg["data"])
                        self._batches += 1
                        self._arrived.notify_all()
        except ConnectionClosedOK:
            pass
        except Exception as error:
            with self._lock:
                self._error = error
                self._arrived.notify_all()

    def _stop(self):
        # Closing the socket from this thread ends the loop of the reading thread.
        self._connection.close()
        if self._thread is not None:
            self._thread.join(timeout=5)
        self._client.call("deleteMonitoring", path={"topic": self.topic}, check=False)
