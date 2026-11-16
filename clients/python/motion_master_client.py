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
stream_monitoring() and notifications() stream from the WebSocket. monitoring() creates a
monitoring, records its rows while a with block runs, and deletes it at the
end, so a script can check the rows afterwards. read_value() reads one object,
decoded by the server, and wait_for_value() reads it until a condition holds.
Profile, Cia402 and Somanet name every object of a SOMANET device as an address,
such as Cia402.STATUSWORD, generated in motion_master_objects.py. AlState names the
AL states, Cia402State the CiA402 states, and Cia402.state() names the state a
statusword reports. OperationMode names the operation modes.
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
import signal
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


class OperationMode(enum.IntEnum):
    """An operation mode, in the encoding of 0x6060 and 0x6061.

    It is an int, so a member goes into a request body as it is, and OperationMode(value) converts
    a mode that the server reports. The values from 0 up are the CiA402 modes. The negative values
    are the SOMANET modes. Value 5 does not exist, because CiA402 reserves it.
    """

    NO_MODE = 0
    PROFILE_POSITION = 1
    VELOCITY = 2
    PROFILE_VELOCITY = 3
    PROFILE_TORQUE = 4
    HOMING = 6
    INTERPOLATED_POSITION = 7
    CYCLIC_SYNC_POSITION = 8
    CYCLIC_SYNC_VELOCITY = 9
    CYCLIC_SYNC_TORQUE = 10
    CYCLIC_SYNC_TORQUE_COMMUTATION_ANGLE = 11
    IMPEDANCE = -6
    JOINT_TORQUE = -5
    SYSTEM_IDENTIFICATION = -4  # The firmware deprecates this mode.
    OPEN_LOOP_FIELD = -3
    DIAGNOSTICS = -2
    COGGING_COMPENSATION_RECORDING = -1


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


def _file_path(position, name):
    """Return the path parameters of a file of a device, with the name percent-encoded.

    The server decodes the name, so a name with a space or a slash names the file it means.
    """
    return _path(position, filename=urllib.parse.quote(name, safe=""))


def _user_cache_path(path):
    """Return the path parameters of a file of the user cache, with each segment percent-encoded.

    The "/" between the folders stays, because the server reads a nested path from the segments.
    """
    return {"path": urllib.parse.quote(path, safe="/")}


_FORMATS = ("bytes", "text", "json", "response")


def _check_format(format):
    if format not in _FORMATS:
        raise ValueError(f"format must be bytes, text, json or response, not {format!r}")


def _decode(response, format):
    """Return the body of a file read in `format`: bytes, text, JSON, or the response itself."""
    if format == "response":
        return response
    if format == "text":
        return response.content.decode("utf-8")
    if format == "json":
        return json.loads(response.content)
    return response.content


_CONFIG_TEMPLATE_URL = (
    "https://github.com/synapticon/motion-master/blob/main/clients/python/config.example.toml"
)


def _missing_config_message(candidates):
    """Say where from_config() looked, and how to make the file."""
    looked = " or ".join(str(c) for c in candidates)
    # A clone has the template next to this module. A pip install does not.
    template = Path(__file__).resolve().parent / "config.example.toml"
    if template.exists():
        make = f"copy the template: cp {template} {candidates[-1]}"
    else:
        make = f"write config.toml in the current directory from {_CONFIG_TEMPLATE_URL}"
    return f"no config.toml in {looked} -- {make}"


def _not_reached(result, step):
    """Say which devices of a state change did not reach `step`, and why."""
    failed = ", ".join(
        f"device {d['slavePosition']} in {AlState(d['alState']).name}"
        f" with alStatusCode 0x{d['alStatusCode']:04X}"
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


class MonitoringError(Exception):
    """A recording of monitoring() fails.

    The server refuses the monitoring, no row arrives, the rows of the last flush do not arrive, or
    the WebSocket closes during the recording.
    """


class ProcessDataDumpError(Exception):
    """The server could not produce a process-data dump. The message is the server's reason."""


# The one topic that every notification source of the server publishes to.
NOTIFICATION_TOPIC = "notifications"

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
        return response.text.strip() or response.reason


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
        # The separators of JSON.stringify, so that both clients print the same line.
        line += f"  {json.dumps(step['value'], ensure_ascii=False, separators=(',', ':'))}"
    if "error" in step:
        line += f"  {step['error']}"
    return line


def wire_time_ms(response):
    """Return the wire time of a request in milliseconds, or None when the server reported none.

    The server reports the time that a fieldbus operation spent on the device in the standard
    Server-Timing header, as the metric wire: "Server-Timing: wire;dur=1.500" is 1.5 ms.
    """
    header = response.headers.get("Server-Timing")
    if header is None:
        return None
    for entry in header.split(","):
        metric, *parameters = (part.strip() for part in entry.split(";"))
        if metric != "wire":
            continue
        for parameter in parameters:
            key, _, value = (part.strip() for part in parameter.partition("="))
            if key == "dur":
                try:
                    return float(value)
                except ValueError:
                    return None
        return None
    return None


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
    user_errors = (
        StateError,
        ProcedureError,
        CaptureError,
        MonitoringError,
        ProcessDataDumpError,
        FileNotFoundError,
        ConnectionError,
        TimeoutError,
    )
    if isinstance(error, user_errors):
        return str(error)
    return None


def run(main):
    """Call main() and exit with one line on stderr when it fails with an error a user can act on.

    Those errors are: no connection to the server, an HTTP error status, a WebSocket that closes, a
    StateError, a ProcedureError, a CaptureError, a MonitoringError, a ProcessDataDumpError, a
    TimeoutError of wait_for_value() and a missing file. Any other error is a bug, so it keeps its
    traceback. Set MM_TRACEBACK=1 to get the traceback for every error.
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

    def __init__(self, http_url, ws_url=None, verify=False, fieldbus=None, spec_copy=None):
        """Read the spec of the server at http_url.

        spec_copy names a file that receives a copy of the spec, for a reader or a coding assistant
        that looks up an operationId without a running server. from_config() puts it beside the
        config file, because an installed module can sit in a directory nobody may write.
        """
        self._http_url = http_url.rstrip("/")
        # The body of POST /api/init, for example {"driver": "soem", "adapter": "eth0"}.
        self.fieldbus = fieldbus
        # The WebSocket endpoint. stream_monitoring(), notifications() and monitoring() connect to
        # it, so a script needs no second connection object.
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
        if spec_copy is not None:
            Path(spec_copy).write_text(response.text, encoding="utf-8")
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
        """Build a client from a TOML config file.

        Without a path, it reads config.toml in the current directory, and else config.toml next
        to this module. The first is for a client installed with pip. The second is for a clone of
        the repository, where a script runs from any directory. The file gives the host, the two
        ports, the TLS verification and the [fieldbus] table that init() sends.
        config.example.toml is the template. The client writes the spec of the server to
        swagger.yml beside the file.
        """
        if path:
            path = Path(path)
            if not path.exists():
                raise FileNotFoundError(f"{path} not found")
        else:
            here = Path(__file__).resolve().parent
            # One entry when the current directory is the directory of this module.
            candidates = list(dict.fromkeys([Path.cwd() / "config.toml", here / "config.toml"]))
            path = next((c for c in candidates if c.exists()), None)
            if path is None:
                raise FileNotFoundError(_missing_config_message(candidates))
        with open(path, "rb") as f:
            cfg = tomllib.load(f)
        return cls(
            http_url=f"https://{cfg['host']}:{cfg['http_port']}",
            ws_url=f"wss://{cfg['host']}:{cfg['ws_port']}",
            verify=cfg.get("verify", False),
            fieldbus=cfg.get("fieldbus"),
            spec_copy=path.parent / "swagger.yml",
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

    def export_eni(self):
        """Export the bus as an ENI document (exportEni), with the parts the server could not read.

        Returns {"xml": the document as text, "warnings": the number of parts that were asked for
        and not answered}. A bus that has not reached SAFEOP or OP answers 409, because before that
        there is no mapping to describe. Raises requests.HTTPError when the server refuses.
        """
        r = self.call("exportEni", headers={"Accept": "application/xml"})
        try:
            warnings = int(r.headers.get("X-Eni-Warnings", "0"))
        except ValueError:
            warnings = 0
        # The server names no charset, and an ENI is UTF-8. r.text would guess the encoding.
        return {"xml": r.content.decode("utf-8"), "warnings": warnings}

    def parse_eni(self, xml):
        """Read an ENI document (parseEni) and return its network, with each datagram annotated.

        Any ENI that follows ETG.2100 is read, not only one that export_eni() wrote. Raises
        requests.HTTPError when the server refuses.
        """
        body = xml.encode("utf-8")
        headers = {"Content-Type": "application/xml"}
        return self.call("parseEni", data=body, headers=headers).json()

    def parse_esi(self, xml, modules=None, format="json"):
        """Read an ESI document (parseEsi) and return every device it describes.

        Each device comes with its own object table. No device is involved. `xml` is a str, or
        bytes, because an ESI may declare an encoding other than UTF-8. `modules` limits the merge
        to these ModuleIdent values, where a slot offers a choice; without it, every module that a
        slot references is merged. `format` says what to return: "json", the parsed result (the
        default), or "response", the requests.Response, for its headers, such as the time the
        server spent, which wire_time_ms() reads.
        """
        if format not in ("json", "response"):
            raise ValueError(f"format must be json or response, not {format!r}")
        data = xml.encode("utf-8") if isinstance(xml, str) else xml
        query = {"modules": ",".join(str(m) for m in modules)} if modules else None
        response = self._post_bytes("parseEsi", data, "application/xml", query=query)
        return response if format == "response" else response.json()

    def parse_integro_variant(self, data):
        """Decode the .variant file of an Integro (parseIntegroVariant). No device is involved."""
        return self._post_bytes("parseIntegroVariant", data).json()

    def parse_ic_mu_restore_file(self, data):
        """Decode an iC-MU restore file (parseIcMuRestoreFile). No device is involved.

        The file is the circulo_integrated_encoder_<port>_config.bin that a configuration writes.
        """
        return self._post_bytes("parseIcMuRestoreFile", data).json()

    def list_user_cache_files(self):
        """Return the files in the user cache of the server (listUserCacheFiles).

        The result is {"root": the folder on the computer of the server, "files": [...]}, and each
        file is {"path": relative to the root, "size": ..., "modifiedMs": ...}.
        """
        return self.call("listUserCacheFiles").json()

    def read_user_cache_file(self, path, format="bytes"):
        """Read a file of the user cache (getUserCacheFile).

        `path` is relative to the root, with "/" between the folders, such as
        "dumps/recorder-1.mmpd". `format` says what to return, as for read_file(): "bytes" (the
        default), "text", "json", or "response", the requests.Response, for its headers, such as
        the time the server spent, which wire_time_ms() reads.
        """
        _check_format(format)
        return _decode(self.call("getUserCacheFile", path=_user_cache_path(path)), format)

    def write_user_cache_file(self, path, content):
        """Write a file into the user cache (putUserCacheFile), and replace a file of the same path.

        The server creates the folders of the path. `content` is bytes, or a str, which is written as
        UTF-8. Returns the requests.Response, for its headers.
        """
        data = content.encode("utf-8") if isinstance(content, str) else bytes(content)
        return self.call(
            "putUserCacheFile",
            path=_user_cache_path(path),
            data=data,
            headers={"Content-Type": "application/octet-stream"},
        )

    def remove_user_cache_file(self, path):
        """Remove a file from the user cache (deleteUserCacheFile)."""
        self.call("deleteUserCacheFile", path=_user_cache_path(path))

    def list_parameter_cache_entries(self):
        """Return the entries of the parameter cache (listParameterCacheEntries).

        Each entry is one object dictionary per vendor, product and firmware version, which a scan
        of such a device reads from the cache instead of from the device.
        """
        return self.call("listParameterCacheEntries").json()

    def read_parameter_cache_entry(self, id, format="json"):
        """Read one entry of the parameter cache (getParameterCache).

        `id` is the one that list_parameter_cache_entries() gives. `format` says what to return:
        "json", the parsed file (the default), or "bytes", the file as the server keeps it.
        """
        if format not in ("json", "bytes"):
            raise ValueError(f"format must be json or bytes, not {format!r}")
        return _decode(self.call("getParameterCache", path={"id": id}), format)

    def remove_parameter_cache_entry(self, id):
        """Remove one entry of the parameter cache (deleteParameterCache).

        The next scan of such a device reads its object dictionary from the device again.
        """
        self.call("deleteParameterCache", path={"id": id})

    def server_log(self):
        """Return the log of the server (getLog), the entries it keeps in memory, oldest first."""
        return self.call("getLog").content.decode("utf-8")

    def _post_bytes(self, operation_id, data, content_type="application/octet-stream", query=None):
        headers = {"Content-Type": content_type}
        return self.call(operation_id, data=bytes(data), headers=headers, query=query)

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

    def write_value(self, position: int, obj: ObjectAddress[_T], value: _T) -> _T:
        """Write one object, converted by the server to its data type, and return the value read back.

        The twin of read_value(). The server writes a PDO-mapped object of an exchanging device into
        the process data, and any other object with an SDO download.
        """
        index, subindex = obj
        path = _path(position, index=index, subindex=subindex)
        return self.call("writeParameter", path=path, body={"value": value}).json()["value"]

    def read_sdo(self, position, index, subindex):
        """Return the raw bytes of one object, read with an SDO upload (sdoUpload).

        read_value() decodes the value by its data type instead, and int.from_bytes(data, "little")
        turns bytes into an integer.
        """
        path = _path(position, index=index, subindex=subindex)
        return bytes(self.call("sdoUpload", path=path).json()["data"])

    def write_sdo(self, position, index, subindex, data):
        """Write raw bytes, a bytes object or a list of ints, to one object with an SDO download."""
        path = _path(position, index=index, subindex=subindex)
        self.call("sdoDownload", path=path, body={"data": list(data)})

    def list_files(self, position):
        """Return the files on a device (listDeviceFiles), each with the size the device reports.

        Each file is {"name": ..., "byteCount": ...}. byteCount is missing when the device reports a
        name without a size.
        """
        return self.call("listDeviceFiles", path=_path(position)).json()

    def read_file(self, position, name, format="bytes"):
        """Read a file from a device over FoE (foeReadFile).

        `format` says what to return: "bytes" (the default), "text", the file decoded as UTF-8, such
        as a CSV, "json", the file parsed as JSON, or "response", the requests.Response, for its
        headers, such as the wire time that wire_time_ms() reads.
        """
        _check_format(format)
        return _decode(self.call("foeReadFile", path=_file_path(position, name)), format)

    def write_file(self, position, name, content):
        """Write a file to a device over FoE (foeWriteFile), and replace a file of the same name.

        `content` is bytes, or a str, which is written as UTF-8. Returns the requests.Response, for its
        headers.
        """
        data = content.encode("utf-8") if isinstance(content, str) else bytes(content)
        return self.call(
            "foeWriteFile",
            path=_file_path(position, name),
            data=data,
            headers={"Content-Type": "application/octet-stream"},
        )

    def remove_file(self, position, name):
        """Remove a file from the flash of a device (deleteDeviceFile)."""
        self.call("deleteDeviceFile", path=_file_path(position, name))

    def read_sii(self, position, format="json"):
        """Read the SII of a device, the configuration in its EEPROM (readSii).

        `format` says what to return: "json", the parsed SII (the default), or "bytes", the raw
        EEPROM image.
        """
        if format not in ("json", "bytes"):
            raise ValueError(f"format must be json or bytes, not {format!r}")
        if format == "json":
            return self.call("readSii", path=_path(position)).json()
        # The Accept header selects the raw image over the parsed JSON.
        headers = {"Accept": "application/octet-stream"}
        return self.call("readSii", path=_path(position), headers=headers).content

    def write_sii(self, position, image):
        """Write a raw SII image to the EEPROM of a device (writeSii).

        A wrong image can leave the device unidentifiable, and the device takes the new image only
        after a power cycle.
        """
        self.call(
            "writeSii",
            path=_path(position),
            data=bytes(image),
            headers={"Content-Type": "application/octet-stream"},
        )

    def parse_sii(self, image):
        """Decode a raw SII image (parseSii), such as a file that read_sii() saved.

        Returns the parsed SII. No device is involved.
        """
        return self._post_bytes("parseSii", image).json()

    def read_hrd_recording(self, position, data, format="json"):
        """Read the high resolution data recording of a device (readHrdRecording).

        The recording is the files that the hrd-streaming procedure left on the drive, decoded as
        `data` says: "encoder-raw" or "system-identification". `format` says what to return:
        "json", the decoded recording (the default), or "csv", the recording as CSV text, a header
        row of column names and then one row per sample.
        """
        if format not in ("json", "csv"):
            raise ValueError(f"format must be json or csv, not {format!r}")
        query = {"data": data}
        if format == "json":
            return self.call("readHrdRecording", path=_path(position), query=query).json()
        # The Accept header selects the CSV over the JSON.
        headers = {"Accept": "text/csv"}
        r = self.call("readHrdRecording", path=_path(position), query=query, headers=headers)
        return r.content.decode("utf-8")

    def wait_for_value(
        self,
        position: int,
        obj: ObjectAddress[_T],
        predicate: Callable[[_T], object],
        timeout_ms: int = 1000,
        interval_ms: int = 50,
    ) -> _T:
        """Read an object until `predicate(value)` is true, and return that value.

        `obj` is an address, for example Cia402.STATUSWORD, or (index, subindex). Each read is
        read_value(), so a PDO-mapped object of an exchanging device costs no bus transfer.

            client.wait_for_value(1, Cia402.STATUSWORD, lambda sw: sw & 0x0400)  # target reached

        Raises TimeoutError with the last value when the predicate is still false after
        `timeout_ms`.
        """
        deadline = time.monotonic() + timeout_ms / 1000
        while True:
            value = self.read_value(position, obj)
            if predicate(value):
                return value
            if time.monotonic() >= deadline:
                index, subindex = obj
                raise TimeoutError(
                    f"device {position} 0x{index:04X}:{subindex:02X} is {value}"
                    f" after {timeout_ms} ms"
                )
            time.sleep(interval_ms / 1000)

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

        The mode is an OperationMode, such as OperationMode.CYCLIC_SYNC_VELOCITY, or its int value.

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

    def run_procedure(self, position, name, params=None, poll_interval_ms=500):
        """Start a procedure on a device and yield its snapshot at every poll until the run ends.

        The last snapshot yielded has the final status: succeeded, failed or cancelled. Polling is
        lossless, because every finished step keeps its status and value, so a step that starts
        and ends between two polls still shows. When the caller stops iterating before the run
        ends, for example on Ctrl-C, the run is cancelled, so that the drive does not continue
        alone. `poll_interval_ms` is the time between two polls.

        Raises ProcedureError when the server refuses the start: 409 when another procedure runs
        on the device, 404 when the device has no procedure of that name, 400 for a parameter the
        procedure refuses.
        """
        path = _path(position, procedureName=name)
        # A Ctrl-C while the start is on the way would leave a run going that this generator never
        # cancels, so it waits for the answer and is raised in the loop below, where the run is
        # cancelled. A second Ctrl-C stops at once, for a server that never answers. Python takes a
        # SIGINT handler only in the main thread.
        interrupted = []

        def on_sigint(*_):
            if interrupted:
                raise KeyboardInterrupt
            interrupted.append(True)

        previous = None
        if threading.current_thread() is threading.main_thread():
            previous = signal.signal(signal.SIGINT, on_sigint)
        try:
            r = self.call("startProcedure", path=path, body=params or {}, check=False)
        finally:
            if previous is not None:
                signal.signal(signal.SIGINT, previous)
        if not r.ok:
            raise ProcedureError(
                f"{name} on device {position}: {_reason(r)} (HTTP {r.status_code})"
            )
        snapshot = r.json()
        try:
            if interrupted:
                raise KeyboardInterrupt
            while True:
                yield snapshot
                if snapshot["status"] != "running":
                    return
                time.sleep(poll_interval_ms / 1000)
                snapshot = self.call("getProcedure", path=path).json()
        finally:
            if snapshot["status"] == "running":
                log.info("cancelling %s on device %d", name, position)
                # A failed cancel has nothing to add to the error or the early exit that brought
                # the run here, and must not replace it.
                try:
                    self.call("cancelProcedure", path=path, check=False)
                except requests.RequestException:
                    pass

    def notifications(self):
        """Subscribe to the notifications topic and yield the data of each notification.

        Every notification source publishes to the one "notifications" topic, so one subscription
        receives every event, also a kind that a later server adds. data["event"] names the kind.
        The caller stops by breaking out of the loop. A close of the WebSocket raises
        ConnectionError, because the notifications sent meanwhile are lost.
        """
        for msg in self._subscribe(NOTIFICATION_TOPIC):
            if msg.get("type") == "notification":
                yield msg["data"]

    def _subscribe(self, topic):
        """Open the WebSocket, subscribe to a topic, and yield every message as parsed JSON."""
        if not self.ws_url:
            raise RuntimeError("no ws_url configured on this client")

        # Lazy import so HTTP-only usage doesn't require the websockets package.
        from websockets.exceptions import ConnectionClosed
        from websockets.sync.client import connect

        with connect(self.ws_url, ssl=self._ssl_context()) as ws:
            ws.send(json.dumps({"subscribe": topic}))
            try:
                # Only the caller ends the stream, by breaking out of the loop. A close of the
                # WebSocket loses the messages sent meanwhile, so it is an error.
                try:
                    for message in ws:
                        yield json.loads(message)
                except ConnectionClosed as error:
                    raise ConnectionError("the WebSocket closed") from error
                raise ConnectionError("the WebSocket closed")
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

    def stream_monitoring(self, topic):
        """Subscribe to the topic of a monitoring and yield each batch of rows from the WebSocket.

        The monitoring must exist already: create it with client.call("createMonitoring", ...).
        To record a monitoring for a span instead, use monitoring(), which creates and deletes it.
        A batch is a list of rows, [timestampUs, v0, v1, ...], with the values in the order of the
        parameters of the monitoring. The stream is lossless: a batch holds every cycle recorded
        since the batch before. The caller stops by breaking out of the loop, and that closes the
        WebSocket. A close of the WebSocket raises ConnectionError, because the batches sent
        meanwhile are lost.
        """
        for msg in self._subscribe(topic):
            if msg.get("type") == "monitoring" and msg.get("topic") == topic:
                yield msg["data"]

    def monitoring(self, parameters, interval_ms=50, topic=None, name=None):
        """Return a MonitoringRecording of the rows of the parameters while a with block runs.

        `parameters` is a list of (devicePosition, index, subindex). Create it after the devices
        are in SAFEOP or OP, because the server decides once, at creation, whether each object
        comes from the process data or from SDO reads. Without `topic`, a unique one is chosen.

            with client.monitoring([(1, *Cia402.VELOCITY_ACTUAL_VALUE)]) as m:
                ...  # the move
            velocity = m.column(1, Cia402.VELOCITY_ACTUAL_VALUE)
        """
        topic = topic or f"py-{uuid.uuid4().hex[:12]}"
        return MonitoringRecording(self, parameters, interval_ms, topic, name)


@functools.cache
def _object_names():
    """Map (index, subindex) to the generated name of every object, for example STATUSWORD."""
    names = {}
    for group in (Profile, _Cia402Objects, Somanet):
        for name, value in vars(group).items():
            if isinstance(value, ObjectAddress):
                names.setdefault(tuple(value), name)
    return names


def object_name(index, subindex):
    """Return the generated name of an object, such as STATUSWORD, or its address when it has none."""
    return _object_names().get((index, subindex), object_address_text(index, subindex))


def object_address_text(index, subindex):
    """Return the address of an object in hex, such as 0x6041:00."""
    return f"0x{index:04X}:{subindex:02X}"


class MonitoringRecording:
    """The recording of a monitoring: the monitoring on the server, and every row of the with block.

    Entering the block creates the monitoring, subscribes to its topic, and returns when the first
    row arrives, so nothing the block does is missed. Leaving the block waits for the rows of the
    last flush, closes the WebSocket and deletes the monitoring, also when the block raised. The
    rows stay readable after the block.

    Raises MonitoringError when the server refuses the monitoring, when no row arrives within
    5000 ms, when the rows of the last flush do not arrive, and when the WebSocket closes during the
    recording. An error of the block wins over them.
    """

    # How long entering waits for the first row. Leaving waits two flush intervals for the last
    # rows, and this margin on top.
    _FIRST_ROW_TIMEOUT_MS = 5000
    _LAST_FLUSH_MARGIN_MS = 2000

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
        # Set before _stop() closes the WebSocket, so that the reading thread takes that close for
        # the end of the recording, and any other close for an error.
        self._stopping = False
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
        raise KeyError(f"device {position} 0x{index:04X}:{subindex:02X} is not in this monitoring")

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

    @property
    def names(self):
        """The name of each parameter's column, in order: its device position and its object.

        The object is its generated name, 1:STATUSWORD, or 1:0x2110:03 for an object with no
        generated name. write_csv() and print_table() name their columns with these.
        """
        return [
            f"{p['devicePosition']}:{object_name(p['index'], p['subindex'])}"
            for p in self.parameters
        ]

    def write_csv(self, path):
        """Write every row to a CSV file at `path`, and create its folder when it is missing.

        The columns are time_s, the seconds from the first row; timestamp_us, the time on the
        server when the cycle ran, in epoch microseconds; then one column per parameter, named as
        in `names`. A value is empty in a cycle where its device did not exchange process data.
        """
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        with self._lock:
            rows = list(self.rows)
        start = rows[0][0] if rows else 0
        with path.open("w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["time_s", "timestamp_us", *self.names])
            for timestamp_us, *values in rows:
                times = [f"{(timestamp_us - start) / 1e6:.6f}", timestamp_us]
                writer.writerow(times + ["" if v is None else v for v in values])

    def print_table(self, every_ms=100, file=None):
        """Print the rows as a table: the first row, then one row every `every_ms`.

        The columns are time_s, the seconds from the first row, then one column per parameter,
        named as in `names`. A Statusword (0x6041) is printed in hex, and its column is followed by
        a column with the CiA402 state it reports. A value is - in a cycle where its device did not exchange
        process data. `file` is where the table goes, sys.stdout by default.
        """
        file = file or sys.stdout
        with self._lock:
            rows = list(self.rows)
        statusword = (Cia402.STATUSWORD.index, Cia402.STATUSWORD.subindex)
        is_statusword = [(p["index"], p["subindex"]) == statusword for p in self.parameters]
        header = ["time_s"]
        for name, sw, p in zip(self.names, is_statusword, self.parameters):
            header.append(name)
            if sw:
                header.append(f"{p['devicePosition']}:CIA402_STATE")
        lines = []
        start = rows[0][0] if rows else 0
        next_us = start
        for timestamp_us, *values in rows:
            if timestamp_us < next_us:
                continue
            next_us = timestamp_us + every_ms * 1000
            line = [f"{(timestamp_us - start) / 1e6:.3f}"]
            for value, sw in zip(values, is_statusword):
                if value is None:
                    line += ["-", "-"] if sw else ["-"]
                elif sw:
                    line += [f"0x{value:04X}", str(Cia402.state(value))]
                else:
                    line.append(str(value))
            lines.append(line)
        widths = [max(len(c) for c in column) for column in zip(header, *lines)]
        for line in [header, *lines]:
            print("  ".join(c.rjust(w) for c, w in zip(line, widths)), file=file)

    def __enter__(self):
        body = {"topic": self.topic, "interval": self._interval_ms, "parameters": self._request}
        if self._name is not None:
            body["name"] = self._name
        r = self._client.call("createMonitoring", body=body, check=False)
        if r.status_code != 201:
            raise MonitoringError(
                f"cannot create monitoring {self.topic}: {_reason(r)} (HTTP {r.status_code})"
            )
        self.parameters = r.json()["parameters"]
        try:
            self._start()
        except BaseException:
            self._stop()
            raise
        return self

    def __exit__(self, exc_type, exc, tb):
        flushed = True
        timeout_ms = 2 * self._interval_ms + self._LAST_FLUSH_MARGIN_MS
        try:
            if self._thread is not None and self._thread.is_alive():
                # A row is in a batch only after the next flush, so wait for two more batches.
                with self._lock:
                    target = self._batches + 2
                    flushed = self._arrived.wait_for(
                        lambda: self._batches >= target or self._error is not None,
                        timeout=timeout_ms / 1000,
                    )
        finally:
            self._stop()
        if exc_type is not None:
            return False
        if self._error is not None:
            raise self._error
        if not flushed:
            raise MonitoringError(
                f"monitoring {self.topic}: the rows of the last flush did not arrive within "
                f"{timeout_ms} ms"
            )
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
                lambda: self.rows or self._error is not None,
                timeout=self._FIRST_ROW_TIMEOUT_MS / 1000,
            )
        if self._error is not None:
            raise self._error
        if not arrived:
            raise MonitoringError(
                f"monitoring {self.topic}: no row after {self._FIRST_ROW_TIMEOUT_MS} ms"
            )

    def _read(self):
        cause = None
        try:
            for message in self._ws:
                msg = json.loads(message)
                if msg.get("type") == "monitoring" and msg.get("topic") == self.topic:
                    with self._lock:
                        self.rows.extend(msg["data"])
                        self._batches += 1
                        self._arrived.notify_all()
        except Exception as error:
            cause = error
        with self._lock:
            if not self._stopping:
                self._error = MonitoringError(
                    f"monitoring {self.topic}: the WebSocket closed during the recording"
                )
                self._error.__cause__ = cause
            self._arrived.notify_all()

    def _stop(self):
        # Closing the socket from this thread ends the loop of the reading thread.
        with self._lock:
            self._stopping = True
        self._connection.close()
        if self._thread is not None:
            self._thread.join(timeout=5)
        # A failed delete must not replace the error that ended the recording.
        try:
            self._client.call("deleteMonitoring", path={"topic": self.topic}, check=False)
        except requests.RequestException:
            pass
