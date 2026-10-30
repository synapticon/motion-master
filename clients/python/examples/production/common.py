"""Code that the production scripts share: the log, the questions to the user, the record, and
the steps that every script runs.

A production script runs its steps in order on one drive. Every question, every answer and every
result goes to a log file. The record is a plain text file for the drive, written at the end, and
also when a step fails, with what the script has so far. Both go to the logs/ directory beside
the script, which git ignores, and both have the same name: the build descriptor and the serial
number of the drive, and the start time.
"""

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
    StateError,
    error_line,
)

log = logging.getLogger("production")

HARDWARE_DESCRIPTION_WAIT_S = 10


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

    def connect(self):
        """Open the fieldbus driver, scan, and move the drive to PRE-OP, before the first step.

        The driver is opened when none is open, and the bus is scanned when the last scan is
        missing or wrong. A drive in SAFE-OP or OP stays there.

        A drive with no running firmware does not reach PRE-OP. It goes to BOOT instead, where its
        bootloader serves the files that step 1 reads and the firmware installation writes.
        """
        log.info("")
        log.info("== connect ==")
        self.client.init()
        try:
            self.client.ensure_state(PRE_OP, positions=[self.slave])
        except StateError as e:
            log.info("the drive did not reach PRE-OP: %s", e)
            log.info("moving the drive to BOOT")
            self.client.ensure_state(BOOT, positions=[self.slave])
            self.add("connect", [f"did not reach PRE-OP: {e}", "moved to BOOT"])
        log.info("the drive is in %s", STATE_NAMES[self.al_state()])

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

    def ask(self, question, choices):
        """Ask until the answer is one of `choices`, and log the question and the answer.

        `choices` maps each answer the user types to what it means, and the prompt lists them.
        End of input counts as an exit.
        """
        listed = ", ".join(f"{key} = {meaning}" for key, meaning in choices.items())
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

    def write_record(self, outcome):
        """Write the record of the drive, with the name of the log file and the suffix .txt."""
        path = self._path(".txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write(f"drive: {self.drive_name or 'unknown'}\n")
            f.write(f"build descriptor and serial number: {self.drive_id or 'unknown'}\n")
            f.write(f"started: {self.started.isoformat(timespec='seconds')}\n")
            f.write(f"outcome: {outcome}\n")
            for title, lines in self.sections:
                f.write(f"\n[{title}]\n")
                for line in lines:
                    f.write(f"{line}\n")
        log.info("record written to %s", path)
        return path


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
    # Outside PRE-OP, SAFE-OP and OP the server answers 0x100A from its cache, and the answer does
    # not say so (#177). The version of a firmware that does not run is not recorded.
    if s.firmware_running():
        firmware = s.client.call(
            "readParameter", path={"slavePosition": s.slave, "index": 0x100A, "subindex": 0}
        ).json()["value"]
    else:
        firmware = f"not running (drive in {STATE_NAMES[s.al_state()]})"
    lines.append(f"firmware version: {firmware}")

    for line in lines:
        log.info(line)
    device = hd["device"]
    s.identify(device["name"], f"{device['buildDescriptor']}_{device['serialNumber']}")
    s.add("hardware description", lines)
