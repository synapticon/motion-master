from __future__ import annotations

import logging
import time
from collections.abc import Callable, Iterator

import pytest
from motion_master_client import PRE_OP

from bench import Bench
from report import append_to_job_summary, test_results_markdown, write_device_report

# (test, outcome, message) for each test, in the order they ran.
_results: list[tuple[str, str, str]] = []


@pytest.fixture(scope="session")
def bench(record_testsuite_property: Callable[[str, object], None]) -> Iterator[Bench]:
    """The bench, after one power cycle, so that a run starts from drives that just booted.

    The power supply has no read of its output state, so a power cycle is the only start that
    is the same on every run. The device report is written here, before the first test, to the
    test-suite properties of report.xml and to the job summary. When the last test ends, passed
    or not, the drives are switched off.
    """
    record = record_testsuite_property
    # JUnit counts the setup and the report in the time of the first test, so each one is also
    # recorded alone.
    start = time.monotonic()
    bench = Bench.from_environment()
    try:
        # A cold start: the server forgets every device, the drives restart, and the driver opens.
        bench.reset()
        bench.psu.cycle()
        bench.client.init()
        bench.wait_for_devices_on_wire()
        record("setup.seconds", _seconds_since(start))
        start = time.monotonic()
        _report(bench, record)
        record("report.seconds", _seconds_since(start))
        yield bench
    finally:
        bench.psu.switch(False)


def _report(bench: Bench, record: Callable[[str, object], None]) -> None:
    # The report describes the run, and the tests decide its result. So a report that fails is
    # logged and named in report.xml and the job summary, and the tests still run. PRE-OP is part
    # of the report, because its reads go over the mailbox.
    try:
        bench.client.ensure_state(PRE_OP, exact=True)
        write_device_report(bench, record)
    except Exception as error:
        logging.getLogger(__name__).exception("the device report failed")
        record("report.error", repr(error))
        append_to_job_summary(f"The device report failed: `{error!r}`\n\n")


def _seconds_since(start: float) -> float:
    return round(time.monotonic() - start, 3)


def pytest_runtest_logreport(report: pytest.TestReport) -> None:
    # A test has one result: the call, or the setup when the setup did not pass, because then
    # no call runs. A failed teardown after a passed call is an error too.
    name = report.nodeid.split("::")[-1]
    if report.when == "call" or (report.when == "setup" and not report.passed):
        outcome = "error" if report.when == "setup" and report.failed else report.outcome
        _results.append((name, outcome, _message(report)))
    elif report.when == "teardown" and report.failed:
        _results.append((name, "error", _message(report)))


def pytest_sessionfinish(session: pytest.Session) -> None:
    append_to_job_summary(test_results_markdown(_results))


def _message(report: pytest.TestReport) -> str:
    # A passed test says what it saw, through record_property("message", ...).
    if report.passed:
        return "; ".join(str(v) for k, v in report.user_properties if k == "message")
    crash = getattr(report.longrepr, "reprcrash", None)
    if crash is not None:
        return crash.message
    if isinstance(report.longrepr, tuple):
        return report.longrepr[2]
    return str(report.longrepr).splitlines()[-1]
