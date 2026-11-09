from __future__ import annotations

import logging

import pytest
from motion_master_client import PRE_OP

from bench import Bench
from report import append_to_job_summary, test_results_markdown, write_device_report

# (test, outcome, message) for each test, in the order they ran.
_results: list[tuple[str, str, str]] = []


@pytest.fixture(scope="session")
def bench() -> Bench:
    """The bench, after one power cycle, so that a run starts from drives that just booted.

    The power supply has no read of its output state, so a power cycle is the only start that
    is the same on every run. The device report is written here, before the first test.
    """
    bench = Bench.from_environment()
    bench.power_cycle()
    bench.client.ensure_state(PRE_OP, exact=True)
    # The report describes the run, and the tests decide its result. So a report that fails is
    # logged and named in the job summary, and the tests still run.
    try:
        write_device_report(bench)
    except Exception as error:
        logging.getLogger(__name__).exception("the device report failed")
        append_to_job_summary(f"The device report failed: `{error!r}`\n\n")
    return bench


@pytest.fixture
def fresh(bench: Bench) -> Bench:
    """The bench with the driver closed and no device known. The drives keep their power."""
    bench.reset()
    return bench


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
    if report.passed:
        return ""
    crash = getattr(report.longrepr, "reprcrash", None)
    if crash is not None:
        return crash.message
    if isinstance(report.longrepr, tuple):
        return report.longrepr[2]
    return str(report.longrepr).splitlines()[-1]
