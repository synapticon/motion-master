"""Code that the production scripts share: the log, the questions to the user, the record, and
the steps that every script runs.

A production script runs its steps in order on one drive. Every question, every answer and every
result goes to a log file. The record is a plain text file for the drive, written at the end, and
also when a step fails, with what the script has so far. Both go to the logs/ directory beside
the script, which git ignores, and both have the same name: the build descriptor and the serial
number of the drive, and the start time.
"""

import base64
import logging
import re
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from motion_master_client import (  # noqa: E402
    BOOT,
    OP,
    PRE_OP,
    SAFE_OP,
    STATE_NAMES,
    MotionMasterClient,
    ProcedureError,
    StateError,
    error_line,
    format_step,
    step_changes,
)

log = logging.getLogger("production")

HARDWARE_DESCRIPTION_WAIT_S = 10

# Firmware packages for every production script. The compatibility check of the server tells
# which of them fit a drive.
PACKAGES_DIR = Path(__file__).resolve().parent / "packages"


class Exit(Exception):
    """The user chose to exit, or a step failed and the workflow says to stop."""


class Session:
    """One run of a production script on one drive."""

    def __init__(self, slave, logs):
        self.slave = slave
        self.directory = Path(logs)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.started = datetime.now()
        self.stamp = self.started.strftime("%Y%m%d-%H%M%S")
        self.drive_name = None
        self.drive_id = None
        self.sections = []
        self.appendices = []
        self.log_path = self._path(".log")
        self._setup_logging()
        self.client = MotionMasterClient.from_config()
        log.info("log file %s", self.log_path)

    def _path(self, suffix):
        # Before the hardware description is read, the position on the bus stands in for the
        # build descriptor and the serial number.
        drive_id = self.drive_id or f"device-{self.slave}"
        safe = re.sub(r"[^A-Za-z0-9._-]+", "_", drive_id)
        return self.directory / f"{safe}-{self.stamp}{suffix}"

    def _setup_logging(self):
        # The console gets the message only. The file gets the time too, so that a support log
        # shows how long each step took.
        root = logging.getLogger()
        root.setLevel(logging.INFO)
        console = logging.StreamHandler()
        console.setFormatter(logging.Formatter("%(message)s"))
        root.addHandler(console)
        self._open_log_file()

    def _open_log_file(self):
        self._log_file = logging.FileHandler(self.log_path, encoding="utf-8")
        self._log_file.setFormatter(logging.Formatter("%(asctime)s %(message)s"))
        logging.getLogger().addHandler(self._log_file)

    def identify(self, name, drive_id):
        """Name the drive, and rename the log file to match the record.

        `drive_id` is the build descriptor and the serial number. The full name is too long for a
        file name, so it goes into the record only.
        """
        self.drive_name = name
        self.drive_id = drive_id
        path = self._path(".log")
        if path == self.log_path:
            return
        logging.getLogger().removeHandler(self._log_file)
        self._log_file.close()
        self.log_path = self.log_path.rename(path)
        self._open_log_file()
        log.info("log file renamed to %s", self.log_path)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        """Write the record, also when a step fails.

        A failure goes into the log and into the outcome of the record, then on to the caller, which
        prints it. An Exit ends the run as stopped, and Ctrl-C as cancelled.
        """
        if exc_type is None:
            outcome = "completed"
        elif issubclass(exc_type, Exit):
            outcome = f"stopped: {exc}"
            log.info("stopped: %s", exc)
        elif issubclass(exc_type, KeyboardInterrupt):
            outcome = "cancelled with Ctrl-C"
            log.info("cancelled with Ctrl-C")
        else:
            line = (error_line(exc) if isinstance(exc, Exception) else None) or (
                f"{exc_type.__name__}: {exc}"
            )
            outcome = f"failed: {line}"
            # The caller prints the error to the terminal, so it goes to the file only.
            self._log_file.handle(logging.makeLogRecord({"msg": f"error: {line}"}))
        self.write_record(outcome)
        if exc_type is not None and issubclass(exc_type, KeyboardInterrupt):
            raise SystemExit(1) from None
        return exc_type is not None and issubclass(exc_type, Exit)

    def al_state(self):
        """Return the AL state of the drive, as the server reads it now."""
        query = {"positions": str(self.slave)}
        return self.client.call("getDeviceStates", query=query).json()[0]["alState"]

    def firmware_running(self):
        """Return whether the drive runs its firmware, which is so in PRE-OP, SAFE-OP and OP."""
        return self.al_state() in (PRE_OP, SAFE_OP, OP)

    def heading(self, number, title):
        """Show and log the start of a step."""
        log.info("")
        log.info("== step %d: %s ==", number, title)

    def ask(self, question, choices, listed=None):
        """Ask until the answer is one of `choices`, and log the question and the answer.

        `choices` maps each answer the user types to what it means, and the prompt lists them.
        `listed` replaces that list in the prompt, for choices already shown above it. End of
        input counts as an exit.
        """
        listed = listed or ", ".join(f"{key} = {meaning}" for key, meaning in choices.items())
        log.info("question: %s (%s)", question, listed)
        while True:
            try:
                answer = input(f"{question} [{listed}]: ").strip().lower()
            except EOFError:
                log.info("answer: end of input")
                raise Exit("no answer: end of input") from None
            if answer in choices:
                log.info("answer: %s (%s)", answer, choices[answer])
                return answer
            print(f"type one of: {', '.join(choices)}")

    def add(self, title, lines):
        """Add a section to the record."""
        self.sections.append((title, list(lines)))

    def append(self, title, lines):
        """Add a section to the end of the record, after every step's section.

        For long content, such as a whole file, so that it does not push the results apart.
        """
        self.appendices.append((title, list(lines)))

    def write_record(self, outcome):
        """Write the record of the drive, with the name of the log file and the suffix .txt."""
        path = self._path(".txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write(f"drive: {self.drive_name or 'unknown'}\n")
            f.write(f"build descriptor and serial number: {self.drive_id or 'unknown'}\n")
            f.write(f"started: {self.started.isoformat(timespec='seconds')}\n")
            f.write(f"outcome: {outcome}\n")
            for title, lines in self.sections + self.appendices:
                f.write(f"\n[{title}]\n")
                for line in lines:
                    f.write(f"{line}\n")
        log.info("record written to %s", path)
        return path


def connect(s):
    """Open the fieldbus driver, scan, and move the drive to PRE-OP, before the first step.

    The driver is opened when none is open, and the bus is scanned when the last scan is
    missing or wrong. A drive in SAFE-OP or OP stays there.

    A drive with no running firmware does not reach PRE-OP. It goes to BOOT instead, where its
    bootloader serves the files that step 1 reads and the firmware installation writes.
    """
    log.info("")
    log.info("== connect ==")
    s.client.init()
    try:
        s.client.ensure_state(PRE_OP, positions=[s.slave])
    except StateError as e:
        log.info("the drive did not reach PRE-OP: %s", e)
        log.info("moving the drive to BOOT")
        s.client.ensure_state(BOOT, positions=[s.slave])
        s.add("connect", [f"did not reach PRE-OP: {e}", "moved to BOOT"])
    log.info("the drive is in %s", STATE_NAMES[s.al_state()])


def run_procedure(s, name, params=None):
    """Run a procedure to its end, show and log each step, and return the last snapshot.

    Raises ProcedureError when the server refuses the start, or when the run does not succeed.
    """
    snapshot = None
    changes = step_changes(s.client.run_procedure(s.slave, name, params))
    try:
        for snapshot, changed in changes:
            for step in changed:
                log.info(format_step(step))
    finally:
        # Closing the generator cancels a run that is still in progress, for example on Ctrl-C.
        changes.close()
    if snapshot["status"] != "succeeded":
        reason = f": {snapshot['error']}" if snapshot.get("error") else ""
        raise ProcedureError(f"{name} {snapshot['status']}{reason}")
    return snapshot


def firmware_version(s):
    """Return the firmware version from 0x100A, or say that no firmware runs."""
    # Outside PRE-OP, SAFE-OP and OP the server answers 0x100A from its cache, and the answer does
    # not say so (#177). The version of a firmware that does not run is not recorded.
    if not s.firmware_running():
        return f"not running (drive in {STATE_NAMES[s.al_state()]})"
    path = {"slavePosition": s.slave, "index": 0x100A, "subindex": 0}
    return s.client.call("readParameter", path=path).json()["value"]


def product_lines(title, part):
    """Return the lines that describe one part of a hardware description."""
    lines = [
        f"{title}: {part['name']}",
        f"  build descriptor {part['buildDescriptor']}, serial number {part['serialNumber']}",
    ]
    for c in part.get("components", []):
        version = f", version {c['version']}" if c.get("version") else ""
        lines.append(f"  - {c['name']}, serial number {c['serialNumber']}{version}")
    return lines


def read_hardware_description_file(s):
    """Read the hardware description, and ask again while the drive refuses the FoE read.

    After a power cycle, an Integro refuses FoE reads for about 2.5 s after it reaches PRE-OP,
    while SDO works (#175). Each refusal is logged.
    """
    deadline = time.monotonic() + HARDWARE_DESCRIPTION_WAIT_S
    path = {"slavePosition": s.slave}
    while True:
        r = s.client.call("getDeviceHardwareDescription", path=path, check=False)
        if r.ok or time.monotonic() >= deadline:
            r.raise_for_status()
            return r.json()
        log.info("the drive refused the read, asking again: %s", r.json().get("error", r.text))
        time.sleep(0.5)


def read_hardware_description(s):
    """Step 1: read the hardware description and the firmware version."""
    s.heading(1, "hardware description")
    hd = read_hardware_description_file(s)
    lines = product_lines("device", hd["device"])
    if hd.get("assembly"):
        lines += product_lines("assembly", hd["assembly"])
    lines.append(f"firmware version: {firmware_version(s)}")

    for line in lines:
        log.info(line)
    device = hd["device"]
    s.identify(device["name"], f"{device['buildDescriptor']}_{device['serialNumber']}")
    s.add("hardware description", lines)


def install_firmware(s):
    """Step 2: install a firmware package from the packages directory, or skip the step.

    The server says which package fits the drive. A package that does not fit is installed only
    after a second confirmation, because the installation writes whatever it gets. A drive with no
    running firmware cannot skip the step, because every later step needs the firmware.
    """
    s.heading(2, "firmware")
    descriptors = s.client.call(
        "getDeviceFirmwareDescriptors", path={"slavePosition": s.slave}
    ).json()
    accepted = " or ".join(filter(None, [descriptors["device"], descriptors.get("assembly")]))
    log.info("the drive accepts packages built for %s", accepted)
    before = firmware_version(s)
    log.info("firmware version now: %s", before)

    packages = sorted(PACKAGES_DIR.glob("*.zip"))
    verdicts = []
    for i, package in enumerate(packages, start=1):
        r = s.client.call(
            "checkDeviceFirmwareCompatibility",
            path={"slavePosition": s.slave},
            query={"filename": package.name},
            check=False,
        )
        verdict = r.json() if r.ok else {"compatible": False, "explanation": r.json().get("error")}
        verdicts.append(verdict)
        mark = "fits" if verdict["compatible"] else "DOES NOT FIT"
        log.info("  %d. %s: %s", i, package.name, mark)
    if not packages:
        log.info("no firmware package in %s", PACKAGES_DIR)

    choices = {str(i): package.name for i, package in enumerate(packages, start=1)}
    if s.firmware_running():
        choices["s"] = "skip the step"
    choices["x"] = "exit"
    numbers = f"1-{len(packages)} = package, " if packages else ""
    others = ", ".join(f"{key} = {meaning}" for key, meaning in choices.items() if key in "sx")
    answer = s.ask("install which package?", choices, listed=numbers + others)
    if answer == "x":
        raise Exit("the user chose to exit at the firmware step")
    if answer == "s":
        s.add("firmware", [f"version: {before}", "installation: skipped by the user"])
        return

    package = packages[int(answer) - 1]
    verdict = verdicts[int(answer) - 1]
    if not verdict["compatible"]:
        log.info("%s", verdict["explanation"])
        confirm = s.ask(
            "the package does not fit the drive. install it all the same?",
            {"y": "install", "n": "exit"},
        )
        if confirm == "n":
            raise Exit("the user chose not to install a package that does not fit")

    s.client.call(
        "putUserCacheFile",
        path={"path": f"firmwares/{package.name}"},
        data=package.read_bytes(),
        headers={"Content-Type": "application/octet-stream"},
    )
    log.info("uploaded %s", package.name)
    params = {"packageFilename": package.name, "finalState": PRE_OP}
    run_procedure(s, "firmware-installation", params)
    after = firmware_version(s)
    log.info("firmware version after the installation: %s", after)
    s.add(
        "firmware",
        [
            f"version before: {before}",
            f"package: {package.name}",
            f"verdict: {verdict['explanation']}",
            f"version after: {after}",
        ],
    )


def step_value(snapshot, step_id):
    """Return the value of one step of a finished procedure run, or None."""
    for step in snapshot["steps"]:
        if step["id"] == step_id:
            return step.get("value")
    return None


def load_configuration(s, path):
    """Step 3: load a configuration file into the drive with the config-file-load procedure.

    The server checks every row before it writes anything, writes the file as config.csv, makes the
    drive load it, puts each loaded output value into the process image, and reads every loaded
    object back. The drive must be in Switch On Disabled or Fault, or the server refuses the load.
    """
    s.heading(3, "configuration")
    if not path.exists():
        log.info("no configuration file at %s", path)
        answer = s.ask("skip the step or exit?", {"s": "skip the step", "x": "exit"})
        if answer == "x":
            raise Exit("the user chose to exit at the configuration step")
        s.add("configuration", [f"file: none at {path}", "load: skipped by the user"])
        return

    content = path.read_bytes()
    text = content.decode("utf-8", errors="replace")
    rows = sum(1 for line in text.splitlines() if line.strip().lower().startswith("0x"))
    comments = [line[1:].strip() for line in text.splitlines() if line.startswith("#")]
    log.info("file: %s, %d bytes, %d rows", path, len(content), rows)
    for comment in comments:
        log.info("  # %s", comment)
    log.info("firmware version of the drive: %s", firmware_version(s))

    answer = s.ask(
        "load this configuration?", {"y": "load", "s": "skip the step", "x": "exit"}
    )
    if answer == "x":
        raise Exit("the user chose to exit at the configuration step")
    if answer == "s":
        s.add("configuration", [f"file: {path.name}", "load: skipped by the user"])
        return

    params = {"file": base64.b64encode(content).decode("ascii"), "strategy": "replace"}
    snapshot = run_procedure(s, "config-file-load", params)
    validate = step_value(snapshot, "validate") or {}
    verify = step_value(snapshot, "verify") or {}
    outputs = step_value(snapshot, "outputs") or {}
    lines = [
        f"file: {path.name}",
        f"rows: {validate.get('rows')}, loaded: {validate.get('loads')}",
        f"skipped, unknown object: {', '.join(validate.get('unknownObject', [])) or 'none'}",
        f"skipped, not loadable: {', '.join(validate.get('notLoadable', [])) or 'none'}",
        f"written into the outputs: {', '.join(outputs.get('written', [])) or 'none'}",
    ]
    differing = verify.get("differing", [])
    if differing:
        lines.append("read back with a value other than the file's:")
        for d in differing:
            live = d.get("live", d.get("error"))
            lines.append(f"  {d['address']}: file {d['file']}, drive {live}")
    else:
        lines.append("read back: every loaded value matches the file")
    for line in lines:
        log.info(line)
    s.add("configuration", lines)
    s.append(f"configuration file {path.name}", text.splitlines())
