"""The mailbox and the inputs of the drives in the AL states that exchange process data."""

from __future__ import annotations

import time

import pytest
from motion_master_client import AlState, Profile, Somanet

from bench import Bench

# The drives of the bench with an XMOS EtherCAT service: the Circulo and the Node.
XMOS_DRIVES = ["circulo", "node"]

READS = 50
# A refused or lost request ends at the mailbox timeout of the server, about 700 ms. An answer
# comes much sooner.
MAX_READ_SECONDS = 0.1

# The time the bus gets after a state change before the test reads the working counter, and the
# time between the two reads of the working counter.
SETTLE_SECONDS = 0.5
WKC_WINDOW_SECONDS = 1.0


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


def _change_state(bench: Bench, state: AlState, position: int) -> None:
    result = bench.client.call(
        "changeState", body={"state": state, "positions": [position]}
    ).json()
    assert result["ok"], result


@pytest.mark.parametrize("folder", XMOS_DRIVES)
def test_inputs_stay_live_after_a_drop_from_op_to_safe_op(
    bench: Bench, record_property, folder: str
):
    # The drop from OP to SAFEOP switched off SM2 in a drive, and that also stopped its inputs.
    # The master then saw frozen inputs and a short working counter, although a drive at SAFEOP
    # must send its inputs.
    device = bench.device(folder)
    bench.client.reset()
    bench.client.init()
    bench.client.restart(AlState.SAFEOP)

    mapped = {
        (o["index"], o["subindex"])
        for o in bench.client.call("getProcessImage").json()["inputs"]
        if o["slavePosition"] == device.position
    }
    timestamp = (Somanet.TIMESTAMP.index, Somanet.TIMESTAMP.subindex)
    assert timestamp in mapped, f"0x20F0:00 is not an input of device {device.position}"

    _change_state(bench, AlState.OP, device.position)
    _change_state(bench, AlState.SAFEOP, device.position)
    time.sleep(SETTLE_SECONDS)

    first = bench.client.call("getProcessImage").json()
    before = bench.client.read_value(device.position, Somanet.TIMESTAMP)
    time.sleep(WKC_WINDOW_SECONDS)
    after = bench.client.read_value(device.position, Somanet.TIMESTAMP)
    second = bench.client.call("getProcessImage").json()
    start = time.monotonic()
    bench.client.read_value(device.position, Profile.DEVICE_TYPE)
    sdo_seconds = time.monotonic() - start

    states = bench.client.call("getDeviceStates").json()
    assert states[device.position - 1]["alState"] == AlState.SAFEOP, states
    assert second["lastWkc"] == second["expectedWkc"], (
        f"working counter {second['lastWkc']}, expected {second['expectedWkc']}"
    )
    assert second["shortWkcCycles"] == first["shortWkcCycles"], (
        f"{second['shortWkcCycles'] - first['shortWkcCycles']} cycles with a short working"
        f" counter in {WKC_WINDOW_SECONDS} s"
    )
    # 0x20F0 wraps, so the test checks that it changed, not that it grew.
    assert after != before, f"0x20F0:00 of device {device.position} stayed at {before}"
    assert sdo_seconds <= MAX_READ_SECONDS, f"the SDO read took {sdo_seconds:.3f} s"
    record_property(
        "message",
        f"device {device.position} after OP to SAFEOP: working counter"
        f" {second['lastWkc']} of {second['expectedWkc']}, no short cycle in"
        f" {WKC_WINDOW_SECONDS} s, 0x20F0 {before} -> {after}, SDO read in"
        f" {sdo_seconds * 1000:.1f} ms",
    )
