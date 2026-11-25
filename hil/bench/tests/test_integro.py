"""AL state changes of the Integro, whose netX and SoC each keep their own AL state."""

from __future__ import annotations

import contextlib
import time

import pytest
from motion_master_client import AlState, Cia402, OperationMode

from bench import Bench

# The target velocity of the drop test, in rpm. Low, because the motor coasts down after the drop.
DROP_RPM = 200
# The drive must turn at least this fast before the drop, so that the drop is one while turning.
TURNING_RPM = DROP_RPM // 2
RUN_SECONDS = 1.0
# The time after the drop before the test reads the state of the drive. A cyclic timeout that
# latches a fault does so within it.
AFTER_DROP_SECONDS = 1.0


def _mapping_request(mapping: dict) -> dict:
    """Return a PDO mapping as read (getDevicePdoMapping) in the shape of a write request.

    The read gives each entry its bit offset, which the device derives, so the request omits it.
    """

    def objects(direction: str) -> list[dict]:
        return [
            {
                "pdoIndex": o["pdoIndex"],
                "entries": [
                    {"index": e["index"], "subindex": e["subindex"], "bitLength": e["bitLength"]}
                    for e in o["entries"]
                ],
            }
            for o in mapping[direction]
        ]

    return {"outputs": objects("outputs"), "inputs": objects("inputs")}


def test_integro_takes_a_pdo_mapping_write_at_pre_op_after_safe_op(
    bench: Bench, record_property
):
    # The netX of an Integro did not tell its SoC of a step down from SAFEOP. Back at PREOP the
    # SoC still applied the rules of SAFEOP, and refused a write of an object that is writable at
    # PREOP only, such as the PDO assignment. The test steps down directly, not through INIT,
    # because INIT is what told the SoC.
    integro = bench.device("integro")
    bench.client.reset()
    bench.client.init()
    bench.client.restart(AlState.SAFEOP)
    result = bench.client.call(
        "changeState", body={"state": AlState.PREOP, "positions": [integro.position]}
    ).json()
    assert result["ok"], result

    mapping = bench.client.call("getDevicePdoMapping", path=integro.path).json()
    request = _mapping_request(mapping)
    write = bench.client.call(
        "writeDevicePdoMapping", path=integro.path, body=request, check=False
    )
    assert write.status_code == 200, write.text
    assert _mapping_request(write.json()) == request, write.json()

    # The written mapping must also bring the drive back to SAFEOP.
    result = bench.client.call(
        "changeState", body={"state": AlState.SAFEOP, "positions": [integro.position]}
    ).json()
    assert result["ok"], result
    states = bench.client.call("getDeviceStates").json()
    assert states[integro.position - 1]["alState"] == AlState.SAFEOP, states
    outputs = sum(len(o["entries"]) for o in request["outputs"])
    inputs = sum(len(o["entries"]) for o in request["inputs"])
    record_property(
        "message",
        f"device {integro.position}: SAFEOP to PREOP, the PDO mapping write of"
        f" {len(request['outputs'])} RxPDO ({outputs} entries) and {len(request['inputs'])} TxPDO"
        f" ({inputs} entries) answered 200, and the drive went back to SAFEOP",
    )


def _change_state(bench: Bench, state: AlState, position: int) -> None:
    result = bench.client.call(
        "changeState", body={"state": state, "positions": [position]}
    ).json()
    assert result["ok"], result


@pytest.mark.parametrize("drop_to", [AlState.SAFEOP, AlState.INIT], ids=["safe_op", "init"])
def test_integro_ends_without_fault_after_a_drop_while_turning(
    bench: Bench, record_property, drop_to: AlState
):
    # The netX of an Integro did not tell its SoC of a drop from OP. The SoC still counted itself
    # in OP, got no outputs, and its cyclic timeout latched CyclicHb (0x7500). After the drop the
    # drive must end in Switch on disabled, with no fault.
    integro = bench.device("integro")
    client = bench.client
    client.reset()
    client.init()
    client.restart(AlState.PREOP)
    client.restart(AlState.OP, positions=[integro.position])

    try:
        client.set_operation_mode(integro.position, OperationMode.CYCLIC_SYNC_VELOCITY)
        # A stale target velocity would start the motor at the enable.
        client.set_target(integro.position, "velocity", 0)
        client.enable(integro.position)
        client.set_target(integro.position, "velocity", DROP_RPM)
        time.sleep(RUN_SECONDS)
        velocity = client.read_value(integro.position, Cia402.VELOCITY_ACTUAL_VALUE)
        assert velocity >= TURNING_RPM, f"the motor turns at {velocity} rpm before the drop"

        _change_state(bench, drop_to, integro.position)
        time.sleep(AFTER_DROP_SECONDS)
        if drop_to == AlState.INIT:
            # INIT has no mailbox, so the drive goes to PREOP for the reads.
            _change_state(bench, AlState.PREOP, integro.position)
        statusword = client.read_value(integro.position, Cia402.STATUSWORD)
        error_code = client.read_value(integro.position, Cia402.ERROR_CODE)
    finally:
        # A failed step here must not hide the error of the test.
        with contextlib.suppress(Exception):
            client.restart(AlState.INIT, positions=[integro.position])

    state = Cia402.state(statusword)
    assert state == "SwitchOnDisabled", f"the drive is in {state}, 0x603F is 0x{error_code:04X}"
    assert error_code == 0, f"0x603F is 0x{error_code:04X}"
    record_property(
        "message",
        f"device {integro.position}: {velocity} rpm in CSV, OP to {drop_to.name}: {state},"
        f" 0x603F 0x{error_code:04X}",
    )
