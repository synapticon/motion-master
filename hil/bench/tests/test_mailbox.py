"""The mailbox and the inputs of the drives in the AL states that exchange process data."""

from __future__ import annotations

import time

import pytest
from motion_master_client import AlState, Cia402, Profile, Somanet

from bench import Bench, BenchDevice

# The drives of the bench with an XMOS EtherCAT service: the Circulo and the Node.
XMOS_DRIVES = ["circulo", "node"]
# The drives whose firmware this release changes at SAFEOP: the XMOS drives and the Integro.
SAFE_OP_DRIVES = ["integro", *XMOS_DRIVES]

READS = 50
# A refused or lost request ends at the mailbox timeout of the server, about 700 ms. An answer
# comes much sooner.
MAX_READ_SECONDS = 0.1

# The time the bus gets after a state change before the test reads the working counter, and the
# time between the two reads of the working counter.
SETTLE_SECONDS = 0.5
WKC_WINDOW_SECONDS = 1.0

SDO_READS_AT_PRE_OP = 20


def _change_state(bench: Bench, state: AlState, position: int) -> None:
    result = bench.client.call(
        "changeState", body={"state": state, "positions": [position]}
    ).json()
    assert result["ok"], result


def _assert_timestamp_is_an_input(bench: Bench, device: BenchDevice) -> None:
    mapped = {
        (o["index"], o["subindex"])
        for o in bench.client.call("getProcessImage").json()["inputs"]
        if o["slavePosition"] == device.position
    }
    timestamp = (Somanet.TIMESTAMP.index, Somanet.TIMESTAMP.subindex)
    assert timestamp in mapped, f"0x20F0:00 is not an input of device {device.position}"


def _check_inputs_and_working_counter(bench: Bench, device: BenchDevice) -> str:
    """Check for WKC_WINDOW_SECONDS that the bus has a complete working counter and that the
    inputs of device change. Return what the check saw.
    """
    time.sleep(SETTLE_SECONDS)
    first = bench.client.call("getProcessImage").json()
    before = bench.client.read_value(device.position, Somanet.TIMESTAMP)
    time.sleep(WKC_WINDOW_SECONDS)
    after = bench.client.read_value(device.position, Somanet.TIMESTAMP)
    second = bench.client.call("getProcessImage").json()

    assert second["lastWkc"] == second["expectedWkc"], (
        f"working counter {second['lastWkc']}, expected {second['expectedWkc']}"
    )
    assert second["shortWkcCycles"] == first["shortWkcCycles"], (
        f"{second['shortWkcCycles'] - first['shortWkcCycles']} cycles with a short working"
        f" counter in {WKC_WINDOW_SECONDS} s"
    )
    # 0x20F0 wraps, so the test checks that it changed, not that it grew.
    assert after != before, f"0x20F0:00 of device {device.position} stayed at {before}"
    return (
        f"working counter {second['lastWkc']} of {second['expectedWkc']}, no short cycle in"
        f" {WKC_WINDOW_SECONDS} s, 0x20F0 {before} -> {after}"
    )


@pytest.mark.parametrize("folder", SAFE_OP_DRIVES)
def test_safe_op_from_pre_op_sends_inputs_and_answers_sdo(
    bench: Bench, record_property, folder: str
):
    # A drive at SAFEOP must send its inputs and serve its mailbox. At SAFEOP the server writes
    # the outputs of every drive in each cycle. An XMOS drive that read those outputs only at OP
    # found a process data exchange due on every pass, and served no mailbox request. An Integro
    # wrote its inputs only at OP, so the working counter was short.
    device = bench.device(folder)
    bench.client.reset()
    bench.client.init()
    bench.client.restart(AlState.SAFEOP)
    _assert_timestamp_is_an_input(bench, device)

    inputs = _check_inputs_and_working_counter(bench, device)
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
        f"device {device.position} at SAFEOP: {inputs}; {READS} reads of 0x1000:00, the slowest"
        f" in {max(times) * 1000:.1f} ms",
    )


@pytest.mark.parametrize("folder", SAFE_OP_DRIVES)
def test_inputs_stay_live_after_a_drop_from_op_to_safe_op(
    bench: Bench, record_property, folder: str
):
    # A drop from OP to SAFEOP must keep the inputs of a drive live and end with no fault. An XMOS
    # drive switched off SM2 at the drop, and that also stopped its inputs. An Integro did not
    # tell its SoC of the drop, so the SoC waited for outputs and latched CyclicHb.
    device = bench.device(folder)
    bench.client.reset()
    bench.client.init()
    bench.client.restart(AlState.SAFEOP)
    _assert_timestamp_is_an_input(bench, device)

    _change_state(bench, AlState.OP, device.position)
    _change_state(bench, AlState.SAFEOP, device.position)

    inputs = _check_inputs_and_working_counter(bench, device)
    start = time.monotonic()
    bench.client.read_value(device.position, Profile.DEVICE_TYPE)
    sdo_seconds = time.monotonic() - start
    statusword = bench.client.read_value(device.position, Cia402.STATUSWORD)
    error_code = bench.client.read_value(device.position, Cia402.ERROR_CODE)

    states = bench.client.call("getDeviceStates").json()
    assert states[device.position - 1]["alState"] == AlState.SAFEOP, states
    assert sdo_seconds <= MAX_READ_SECONDS, f"the SDO read took {sdo_seconds:.3f} s"
    cia402_state = Cia402.state(statusword)
    assert cia402_state != "Fault", f"the drive is in Fault, 0x603F is 0x{error_code:04X}"
    assert error_code == 0, f"0x603F is 0x{error_code:04X}"
    record_property(
        "message",
        f"device {device.position} after OP to SAFEOP: {inputs}; SDO read in"
        f" {sdo_seconds * 1000:.1f} ms; {cia402_state}, 0x603F 0x{error_code:04X}",
    )


def test_a_drive_at_pre_op_takes_no_process_data(bench: Bench, record_property):
    # A drive at PREOP must take no process data. The master maps the process image for the whole
    # bus when one drive goes to SAFEOP. A drive that boots into PREOP kept its process data
    # SyncManagers on, so it took outputs meant for the other drives, raised the working counter,
    # and served no mailbox request. The Integro and the Jasper-based drive go to SAFEOP, so that
    # only the drives under test stay at PREOP.
    at_pre_op = [bench.device(folder) for folder in XMOS_DRIVES]
    to_safe_op = [bench.device("integro"), bench.device("ngc")]
    bench.cold_start()
    bench.client.restart(AlState.PREOP)

    result = bench.client.call(
        "changeState",
        body={"state": AlState.SAFEOP, "positions": [d.position for d in to_safe_op]},
    ).json()
    assert result["ok"], result
    time.sleep(SETTLE_SECONDS)

    first = bench.client.call("getProcessImage").json()
    time.sleep(WKC_WINDOW_SECONDS)
    second = bench.client.call("getProcessImage").json()
    slowest = {}
    for device in at_pre_op:
        times = []
        for _ in range(SDO_READS_AT_PRE_OP):
            start = time.monotonic()
            bench.client.read_value(device.position, Profile.DEVICE_TYPE)
            times.append(time.monotonic() - start)
        slowest[device.position] = max(times)

    states = [d["alState"] for d in bench.client.call("getDeviceStates").json()]
    for device in at_pre_op:
        assert states[device.position - 1] == AlState.PREOP, states
    for device in to_safe_op:
        assert states[device.position - 1] == AlState.SAFEOP, states
    assert second["lastWkc"] == second["expectedWkc"], (
        f"working counter {second['lastWkc']}, expected {second['expectedWkc']}"
    )
    assert second["shortWkcCycles"] == first["shortWkcCycles"], (
        f"{second['shortWkcCycles'] - first['shortWkcCycles']} cycles with a short working"
        f" counter in {WKC_WINDOW_SECONDS} s"
    )
    slow = {p: round(t, 3) for p, t in slowest.items() if t > MAX_READ_SECONDS}
    assert not slow, f"the slowest SDO read per device at PREOP: {slow} s"
    reads = ", ".join(f"device {p} in {t * 1000:.1f} ms" for p, t in slowest.items())
    record_property(
        "message",
        f"devices {[d.position for d in to_safe_op]} at SAFEOP, {[d.position for d in at_pre_op]}"
        f" at PREOP: working counter {second['lastWkc']} of {second['expectedWkc']}, no short"
        f" cycle in {WKC_WINDOW_SECONDS} s; slowest of {SDO_READS_AT_PRE_OP} SDO reads: {reads}",
    )
