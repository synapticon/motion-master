"""Code that the production scripts share: the log, the questions to the user, the record, and
the steps that every script runs.

A production script runs its steps in order on one drive. Every question, every answer and every
result goes to a log file. The record is a Markdown file for the drive, written at the end, and
also when a step fails, with what the script has so far. Both go to the logs/ directory beside
the script, which git ignores, and both have the same name: the build descriptor and the serial
number of the drive, and the start time.
"""

import base64
import json
import logging
import os
import re
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from motion_master_client import (  # noqa: E402
    BOOT,
    INIT,
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

# ANSI styles for the console, in the colors of the Synapticon design tokens
# (web/packages/ui/src/theme.css). The log file gets the plain text. NO_COLOR is the common switch
# to turn colors off, and a console that is not a terminal gets none either.
def _rgb(hex_color, bold=False):
    r, g, b = (int(hex_color[i : i + 2], 16) for i in (1, 3, 5))
    return f"\033[{'1;' if bold else ''}38;2;{r};{g};{b}m"


_STYLES = {
    "heading": _rgb("#e0004d", bold=True),  # syn-red
    "mark": _rgb("#e0004d", bold=True),  # syn-red
    "question": "\033[1m",
    "key": _rgb("#00849b", bold=True),  # ocean
    "ok": _rgb("#008d9d"),  # status-good
    "warning": _rgb("#ffcc03"),  # status-warn
    "error": _rgb("#8c072f"),  # status-bad
    "dim": _rgb("#848484"),  # grey-600
}
_RESET = "\033[0m"
_RULE_WIDTH = 78


def _colors(stream):
    return stream.isatty() and not os.environ.get("NO_COLOR")


def paint(text, style, stream=sys.stderr):
    """Return `text` in `style` for `stream`, or plain when the stream gets no colors."""
    if style is None or not _colors(stream):
        return text
    return f"{_STYLES[style]}{text}{_RESET}"


def say(message, *args, style=None, console=None):
    """Log a message. `style` colors it on the console. `console` replaces it on the console only,
    for a line with colors inside it."""
    log.info(message, *args, extra={"style": style, "console": console})


class _ConsoleFormatter(logging.Formatter):
    def format(self, record):
        console = getattr(record, "console", None)
        if console is not None:
            return console
        return paint(record.getMessage(), getattr(record, "style", None))

HARDWARE_DESCRIPTION_WAIT_S = 10

# Firmware packages for every production script. The compatibility check of the server tells
# which of them fit a drive.
PACKAGES_DIR = Path(__file__).resolve().parent / "packages"


def _list_item(line):
    # A record line as a Markdown list item. Two leading spaces nest it one level, and a leading
    # "- " of the line is the item's own marker.
    text = line.lstrip(" ")
    depth = (len(line) - len(text)) // 2
    if text.startswith("- "):
        text = text[2:]
    return f"{'  ' * depth}- {text}"


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
        self.assigned_name = None
        self.firmware_version = None
        self.sections = []
        self.appendices = []
        self._steps = 0
        self.log_path = self._path(".log")
        self._setup_logging()
        self.client = MotionMasterClient.from_config()
        say("log file %s", self.log_path, style="dim")

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
        console.setFormatter(_ConsoleFormatter())
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
        say("log file renamed to %s", self.log_path, style="dim")

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
            say("stopped: %s", exc, style="warning")
        elif issubclass(exc_type, KeyboardInterrupt):
            outcome = "cancelled with Ctrl-C"
            say("cancelled with Ctrl-C", style="warning")
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

    def heading(self, title):
        """Show and log the start of a step, numbered in the order the steps run. Returns the
        number."""
        self._steps += 1
        heading(f"{self._steps} · {title[0].upper()}{title[1:]}")
        return self._steps

    def _file_only(self, message):
        # A question and its answer are on the console already, as the prompt and what was typed.
        self._log_file.handle(logging.makeLogRecord({"msg": message}))

    def ask(self, question, choices, shown=None):
        """Ask until the answer is one of `choices`, and log the question and the answer.

        `choices` maps each answer the user types to what it means. The prompt shows the question
        on one line and the answers below it, each key colored. `shown` replaces those answers with
        other (key, meaning) pairs, such as ("1-6", "package") for a list already printed above.
        End of input counts as an exit.
        """
        shown = shown or list(choices.items())
        self._file_only(
            f"question: {question} ({', '.join(f'{key} = {meaning}' for key, meaning in shown)})"
        )
        options = " · ".join(f"{paint(key, 'key', sys.stdout)} {meaning}" for key, meaning in shown)
        while True:
            answer = self._prompt(question, f"  {options}").lower()
            if answer in choices:
                self._file_only(f"answer: {answer} ({choices[answer]})")
                return answer
            print(paint(f"  type one of: {', '.join(choices)}", "warning", sys.stdout))

    def ask_text(self, question, hint):
        """Ask for a line of text, and log the question and the answer. End of input is an exit."""
        self._file_only(f"question: {question} ({hint})")
        answer = self._prompt(question, paint(f"  {hint}", "dim", sys.stdout))
        self._file_only(f"answer: {answer!r}")
        return answer

    def _prompt(self, question, below):
        # The question, a line below it, and the input line.
        print()
        text = paint(f"{question[0].upper()}{question[1:]}", "question", sys.stdout)
        print(f"{paint('?', 'mark', sys.stdout)} {text}")
        print(below)
        try:
            return input(paint("› ", "mark", sys.stdout)).strip()
        except EOFError:
            self._file_only("answer: end of input")
            raise Exit("no answer: end of input") from None

    def add(self, title, lines):
        """Add a section to the record."""
        self.sections.append((title, list(lines)))

    def append(self, title, lines):
        """Add a section to the end of the record, after every step's section.

        For long content, such as a whole file, so that it does not push the results apart.
        """
        self.appendices.append((title, list(lines)))

    def write_record(self, outcome):
        """Write the record of the drive as Markdown, with the name of the log file and the suffix
        .md: a title, a table of the drive and the run, a section for each step with its lines as
        a list, and each appendix, such as the configuration file, as a code block."""
        path = self._path(".md")
        header = [
            ("Build descriptor and serial number", self.drive_id),
            ("Assigned name", self.assigned_name),
            ("Firmware version", self.firmware_version),
            ("Started", self.started.isoformat(timespec="seconds")),
            ("Outcome", outcome),
        ]
        with open(path, "w", encoding="utf-8") as f:
            f.write(f"# {self.drive_name or f'Device {self.slave}'}\n\n")
            f.write("| | |\n| --- | --- |\n")
            for name, value in header:
                cell = str(value or "unknown").replace("|", "\\|")
                f.write(f"| {name} | {cell} |\n")
            for title, lines in self.sections:
                f.write(f"\n## {title[0].upper()}{title[1:]}\n\n")
                for line in lines:
                    f.write(f"{_list_item(line)}\n")
            for title, lines in self.appendices:
                f.write(f"\n## {title[0].upper()}{title[1:]}\n\n```text\n")
                for line in lines:
                    f.write(f"{line}\n")
                f.write("```\n")
        say("record written to %s", path, style="ok")
        return path


def connect(s):
    """Open the fieldbus driver when none is open, and scan when the last scan is missing or
    wrong. Moves no drive."""
    heading("Connect")
    s.client.init()
    s.client.ensure_scanned()
    say("the drive is in %s", STATE_NAMES[s.al_state()])


def ensure_state(s, state):
    """Make sure the drive is at least in the AL state `state`, as the client's ensure_state does.
    A drive above `state` stays where it is.

    A drive with no running firmware does not reach PRE-OP. It goes to BOOT instead, where its
    bootloader serves the files that the hardware description step reads and the firmware
    installation writes, and stays there whatever `state` is. A drive that reaches PRE-OP but not
    `state` fails the call.
    """
    heading(f"AL state · at least {STATE_NAMES[state]}")
    _reach(s, state, exact=False)


def transition_to_state(s, state):
    """Move the drive to exactly the AL state `state`, down as well as up.

    A drive goes down to PRE-OP or SAFE-OP through INIT. On an Integro, the SoC misses a step down
    from SAFE-OP or OP, and then refuses every write that only PRE-OP allows
    (synapticon/somanet_software#5823). INIT followed by PRE-OP is legal on every drive.

    A drive with no running firmware goes to BOOT, as in ensure_state.
    """
    heading(f"AL state · {STATE_NAMES[state]}")
    ladder = [INIT, PRE_OP, SAFE_OP, OP]
    current = s.al_state()
    down = current in ladder and state in ladder and ladder.index(state) < ladder.index(current)
    if down and state in (PRE_OP, SAFE_OP):
        say("going down through INIT")
        s.client.ensure_state(INIT, positions=[s.slave], exact=True)
    _reach(s, state, exact=True)


def _reach(s, state, exact):
    if state in (INIT, BOOT):
        s.client.ensure_state(state, positions=[s.slave], exact=exact)
    else:
        try:
            s.client.ensure_state(PRE_OP, positions=[s.slave], exact=exact and state == PRE_OP)
        except StateError as e:
            say("the drive did not reach PRE-OP: %s", e, style="warning")
            say("moving the drive to BOOT", style="warning")
            s.client.ensure_state(BOOT, positions=[s.slave])
            s.add("AL state", [f"did not reach PRE-OP: {e}", "moved to BOOT"])
        else:
            s.client.ensure_state(state, positions=[s.slave], exact=exact)
    say("the drive is in %s", STATE_NAMES[s.al_state()])


def heading(label):
    """Show and log a heading: a rule with `label` in it, the width of a line."""
    text = f"━━ {label} "
    say("")
    say(text + "━" * max(3, _RULE_WIDTH - len(text)), style="heading")


def _shorten(value, limit=8):
    # A step value can carry measured data, such as the Bode data of a plant model, with hundreds
    # of numbers. The console shows how many there are, and the log file keeps them all.
    if isinstance(value, dict):
        return {key: _shorten(item, limit) for key, item in value.items()}
    if isinstance(value, list) and len(value) > limit:
        return f"[{len(value)} values]"
    if isinstance(value, list):
        return [_shorten(item, limit) for item in value]
    return value


_STEP_MARKS = {"succeeded": ("✓", "ok"), "failed": ("✗", "error"), "cancelled": ("✗", "warning")}


def _step_line(step):
    # One step of a procedure on the console: a mark for its status, its id, and its value or
    # error. format_step gives the plain line for the log file.
    mark, style = _STEP_MARKS.get(step["status"], ("…", "dim"))
    line = f"  {paint(mark, style)} {step['id']:<24}"
    if "value" in step:
        line += f" {json.dumps(_shorten(step['value']), ensure_ascii=False)}"
    if "error" in step:
        line += f" {paint(step['error'], 'error')}"
    return line


def run_procedure(s, name, params=None):
    """Run a procedure to its end, show and log each step, and return the last snapshot.

    Raises ProcedureError when the server refuses the start, or when the run does not succeed.
    """
    snapshot = None
    changes = step_changes(s.client.run_procedure(s.slave, name, params))
    try:
        for snapshot, changed in changes:
            for step in changed:
                say(format_step(step), console=_step_line(step))
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
        reason = r.json().get("error", r.text)
        say("the drive refused the read, asking again: %s", reason, style="warning")
        time.sleep(0.5)


def read_hardware_description(s):
    """Step 1: read the hardware description and the firmware version."""
    s.heading("hardware description")
    hd = read_hardware_description_file(s)
    lines = product_lines("device", hd["device"])
    if hd.get("assembly"):
        lines += product_lines("assembly", hd["assembly"])
    s.firmware_version = firmware_version(s)
    lines.append(f"firmware version: {s.firmware_version}")

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
    s.heading("firmware")
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
        painted = paint(mark, "ok" if verdict["compatible"] else "error")
        say("  %d. %s: %s", i, package.name, mark, console=f"  {i}. {package.name}: {painted}")
    if not packages:
        say("no firmware package in %s", PACKAGES_DIR, style="warning")

    choices = {str(i): package.name for i, package in enumerate(packages, start=1)}
    if s.firmware_running():
        choices["s"] = "skip the step"
    choices["x"] = "exit"
    shown = [(f"1-{len(packages)}", "package")] if packages else []
    shown += [(key, meaning) for key, meaning in choices.items() if key in ("s", "x")]
    answer = s.ask("install which package?", choices, shown=shown)
    if answer == "x":
        raise Exit("the user chose to exit at the firmware step")
    if answer == "s":
        s.add("firmware", [f"version: {before}", "installation: skipped by the user"])
        return

    package = packages[int(answer) - 1]
    verdict = verdicts[int(answer) - 1]
    if not verdict["compatible"]:
        say("%s", verdict["explanation"], style="warning")
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
    s.firmware_version = after
    say("firmware version after the installation: %s", after)
    s.add(
        "firmware",
        [
            f"version before: {before}",
            f"package: {package.name}",
            f"verdict: {verdict['explanation']}",
            f"version after: {after}",
        ],
    )


def store_parameters(s):
    """Store the drive's parameters to flash, after a step that changed the object dictionary.

    Each such step stores at once, so that a later step that fails leaves the drive with what the
    steps before it did. The drive writes every value it holds into its config.csv. Returns the
    line for the step's section of the record.
    """
    log.info("storing the parameters to flash")
    run_procedure(s, "store-parameters")
    return "parameters stored to flash"


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
    s.heading("configuration")
    if not path.exists():
        say("no configuration file at %s", path, style="warning")
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


def set_assigned_name(s):
    """Step 4: set the assigned name of the drive, 0x20F2, which the user types.

    An empty answer keeps the current name and skips the step. The server refuses a name longer
    than the object holds.
    """
    s.heading("assigned name")
    path = {"slavePosition": s.slave, "index": 0x20F2, "subindex": 0}
    before = s.client.call("readParameter", path=path).json()["value"]
    log.info("assigned name now: %r", before)
    name = s.ask_text("new assigned name?", f"empty keeps {before!r}")
    if not name:
        s.assigned_name = before
        s.add("assigned name", [f"name: {before}", "kept by the user"])
        return

    r = s.client.call("writeParameter", path=path, body={"value": name}, check=False)
    if not r.ok:
        reason = r.json().get("error", r.text) if r.content else r.reason
        raise ProcedureError(f"writing the assigned name failed: {reason}")
    after = s.client.call("readParameter", path=path).json()["value"]
    log.info("assigned name after the write: %r", after)
    if after != name:
        raise ProcedureError(f"the drive holds {after!r} after the write of {name!r}")
    s.assigned_name = after
    s.add("assigned name", [f"before: {before}", f"after: {after}", store_parameters(s)])


def read_object(s, index, subindex):
    """Return the value of one object of the drive."""
    path = {"slavePosition": s.slave, "index": index, "subindex": subindex}
    return s.client.call("readParameter", path=path).json()["value"]


def write_object(s, index, subindex, value):
    """Write one object of the drive, and raise ProcedureError with the server's reason on a
    refusal."""
    path = {"slavePosition": s.slave, "index": index, "subindex": subindex}
    r = s.client.call("writeParameter", path=path, body={"value": value}, check=False)
    if not r.ok:
        reason = r.json().get("error", r.text) if r.content else r.reason
        raise ProcedureError(f"writing 0x{index:04X}:{subindex:02X} failed: {reason}")


def detect_offset(s, method):
    """Detect the commutation offset with the offset-detection procedure.

    The procedure turns the rotor and releases the brake. It needs the drive in OP, so move the
    drive to OP before this step. The drive takes the method from 0x2009:03, which this step sets
    first. Each measurement shows as a step of the procedure. The offset is read back from 0x2001
    and the phase order from 0x2003:05.
    """
    number = s.heading("commutation offset")
    before = read_object(s, 0x2009, 3)
    if before != method:
        write_object(s, 0x2009, 3, method)
    say("offset detection method, 0x2009:03: %s (was %s)", method, before)

    snapshot = run_procedure(s, "offset-detection")
    offset = read_object(s, 0x2001, 0)
    inverted = read_object(s, 0x2003, 5)
    say("commutation offset, 0x2001: %s", offset, style="ok")
    say("motor phases inverted, 0x2003:05: %s", inverted)

    lines = [f"method, 0x2009:03: {method} (was {before})"]
    for step in snapshot["steps"]:
        if "value" in step and step["value"] is not None:
            lines.append(f"{step['id']}: {json.dumps(step['value'], ensure_ascii=False)}")
    lines += [
        f"commutation offset, 0x2001: {offset}",
        f"motor phases inverted, 0x2003:05: {inverted}",
        store_parameters(s),
    ]
    s.add(f"commutation offset, step {number}", lines)


def identify_plant(s, chirp):
    """Identify the plant model with the plant-identification procedure, and show the model.

    The chirp turns the motor in both directions for its whole duration, and needs the drive in
    OP. `chirp` holds the procedure's parameters: startFrequencyMilliHz, targetFrequencyMilliHz,
    targetAmplitudePermil, transitionTimeMs and signalType. The procedure writes the model to
    plant_model.csv on the drive. It sets the operation mode to CST for the chirp and can leave it
    there, but 0x6060 has no backup flag, so a store does not keep it. The step stores nothing. The
    whole model is read back from the drive.
    """
    s.heading("plant model")
    for key, value in chirp.items():
        say("chirp %s: %s", key, value)
    run_procedure(s, "plant-identification", chirp)
    model = s.client.call("getDevicePlantModel", path={"slavePosition": s.slave}).json()
    lines = [
        f"chirp: {json.dumps(model['chirp'], ensure_ascii=False)}",
        f"numerator: {model['numerator']}",
        f"denominator: {model['denominator']}",
        f"inertia: {model['inertia']}",
        f"friction: {model['friction']}",
    ]
    for line in lines:
        say(line, style="ok" if line.startswith(("inertia", "friction")) else None)
    s.add("plant model", lines)


def tune_controller(s, controller, settings):
    """Tune the velocity or the position controller with the controller-auto-tuning procedure.

    `controller` is "velocity" or "position". `settings` holds the other parameters of the
    procedure, such as full and controllerType, and may hold controlStrategy: the position control
    strategy 0x2002, 1 for the simple PID controller and 2 for the cascaded one, which the step
    writes before it tunes. Without it, 0x2002 stays as it is. The procedure needs the plant model
    of the plant model step, computes the gains with the auto-tuning program of the server, and
    writes them. It turns no motor: a full tuning records the velocity noise with the motor at
    rest. The step shows each gain before and after, and stores the parameters to flash.
    """
    s.heading(f"{controller} controller tuning")
    params = {"controller": controller, **settings}
    strategy = params.pop("controlStrategy", None)
    lines = []
    if strategy is not None:
        before = read_object(s, 0x2002, 0)
        if before != strategy:
            write_object(s, 0x2002, 0, strategy)
        lines.append(f"position control strategy, 0x2002: {before} -> {strategy}")
    lines += [f"{key}: {value}" for key, value in params.items()]
    for line in lines:
        say(line)
    snapshot = run_procedure(s, "controller-auto-tuning", params)
    write = step_value(snapshot, "write") or {}
    gains = write.get("gains", [])
    results = [f"{gain['object']}: {gain['before']} -> {gain['after']}" for gain in gains]
    results += [f"skipped, the drive has no {name}" for name in write.get("skipped", [])]
    for line in results:
        say(line, style="ok")
    if write.get("warning"):
        say("%s", write["warning"], style="warning")
        results.append(f"warning: {write['warning']}")
    s.add(f"{controller} controller tuning", lines + results + [store_parameters(s)])


def set_integral_limits(s):
    """Set the integral limits of the controllers with the set-integral-limits procedure.

    The server computes the three limits from the maxima of the drive, writes 0x2011:04,
    0x2012:04 and 0x2012:08, and reports each value before and after. The step shows them, names
    a limit object the drive does not have, and stores the parameters to flash.
    """
    s.heading("integral limits")
    snapshot = run_procedure(s, "set-integral-limits")
    write = step_value(snapshot, "write") or {}
    limits = write.get("limits", [])
    lines = [f"{limit['object']}: {limit['before']} -> {limit['after']}" for limit in limits]
    lines += [f"skipped, the drive has no {name}" for name in write.get("skipped", [])]
    for line in lines:
        say(line, style="ok")
    lines.append(store_parameters(s))
    s.add("integral limits", lines)
