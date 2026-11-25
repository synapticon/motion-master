"""Read objects over the mailbox of the drives in AL states that exchange process data."""

from __future__ import annotations

import time

import pytest
from motion_master_client import AlState, Profile

from bench import Bench

# The drives of the bench with an XMOS EtherCAT service: the Circulo and the Node.
XMOS_DRIVES = ["circulo", "node"]

READS = 50
# A refused or lost request ends at the mailbox timeout of the server, about 700 ms. An answer
# comes much sooner.
MAX_READ_SECONDS = 0.1


@pytest.mark.parametrize("folder", XMOS_DRIVES)
def test_sdo_reads_at_safe_op_answer(bench: Bench, record_property, folder: str):
    # At SAFEOP the server writes the outputs of every drive in each cycle. A drive whose EtherCAT
    # service reads those outputs only at OP finds a process data exchange due on every pass, and
    # then serves no mailbox request.
    device = bench.device(folder)
    bench.client.reset()
    bench.client.init()
    bench.client.restart(AlState.SAFEOP)

    times = []
    for _ in range(READS):
        start = time.monotonic()
        bench.client.read_value(device.position, Profile.DEVICE_TYPE)
        times.append(time.monotonic() - start)

    states = bench.client.call("getDeviceStates").json()
    assert states[device.position - 1]["alState"] == AlState.SAFEOP, states
    slow = [round(t, 3) for t in times if t > MAX_READ_SECONDS]
    assert not slow, f"{len(slow)} of {READS} reads took longer than {MAX_READ_SECONDS} s: {slow}"
    record_property(
        "message",
        f"device {device.position}: {READS} reads of 0x1000:00 at SAFEOP, the slowest in"
        f" {max(times) * 1000:.1f} ms",
    )
