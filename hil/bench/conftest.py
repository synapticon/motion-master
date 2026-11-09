from __future__ import annotations

import pytest
from motion_master_client import PRE_OP

from bench import Bench
from report import write_device_report


@pytest.fixture(scope="session")
def bench() -> Bench:
    """The bench, after one power cycle, so that a run starts from drives that just booted.

    The power supply has no read of its output state, so a power cycle is the only start that
    is the same on every run. The device report is written here, before the first test.
    """
    bench = Bench.from_environment()
    bench.power_cycle()
    bench.client.ensure_state(PRE_OP, exact=True)
    write_device_report(bench)
    return bench


@pytest.fixture
def fresh(bench: Bench) -> Bench:
    """The bench with the driver closed and no device known. The drives keep their power."""
    bench.reset()
    return bench
