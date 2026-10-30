"""Code that the production scripts share: the log, the questions to the user, and the record.

A production script runs its steps in order on one drive. Every question, every answer and every
result goes to a log file. The record is a plain text file for the drive, written at the end, and
also when a step fails, with what the script has so far. Both go to the logs/ directory beside
the script, which git ignores.
"""

import logging
import re
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from motion_master_client import PRE_OP, MotionMasterClient  # noqa: E402

log = logging.getLogger("production")


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
        self.log_path = self.directory / f"production-{self.stamp}.log"
        self.drive_name = None
        self.drive_id = None
        self.sections = []
        self._setup_logging()
        self.client = MotionMasterClient.from_config()
        log.info("log file %s", self.log_path)

    def _setup_logging(self):
        # The console gets the message only. The file gets the time too, so that a support log
        # shows how long each step took.
        root = logging.getLogger()
        root.setLevel(logging.INFO)
        console = logging.StreamHandler()
        console.setFormatter(logging.Formatter("%(message)s"))
        file = logging.FileHandler(self.log_path, encoding="utf-8")
        file.setFormatter(logging.Formatter("%(asctime)s %(message)s"))
        root.addHandler(console)
        root.addHandler(file)

    def connect(self):
        """Open the fieldbus driver, scan, and move the drive to PRE-OP, before the first step.

        The driver is opened when none is open, and the bus is scanned when the last scan is
        missing or wrong. A drive in SAFE-OP or OP stays there.
        """
        log.info("")
        log.info("== connect ==")
        self.client.init()
        self.client.ensure_state(PRE_OP, positions=[self.slave])

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
        """Write the record of the drive, named after the drive and the start time.

        The file name is the build descriptor and the serial number of the drive, because its full
        name is too long for a file name. Before the hardware description is read, the position
        on the bus stands in for them.
        """
        drive_id = self.drive_id or f"device-{self.slave}"
        safe = re.sub(r"[^A-Za-z0-9._-]+", "_", drive_id)
        path = self.directory / f"{safe}-{self.stamp}.txt"
        with open(path, "w", encoding="utf-8") as f:
            f.write(f"drive: {self.drive_name or 'unknown'}\n")
            f.write(f"build descriptor and serial number: {drive_id}\n")
            f.write(f"started: {self.started.isoformat(timespec='seconds')}\n")
            f.write(f"outcome: {outcome}\n")
            for title, lines in self.sections:
                f.write(f"\n[{title}]\n")
                for line in lines:
                    f.write(f"{line}\n")
        log.info("record written to %s", path)
        return path
