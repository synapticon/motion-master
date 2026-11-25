"""Position control near the rollover of 0x6064, and the torque at a change of operation mode.

Both tests turn the motor of a drive. They run on the Integro only, as the motion test does. Each
test puts back every object it changes, also when it fails.
"""

from __future__ import annotations

import contextlib
import logging
import time

import pytest
from motion_master_client import AlState, Cia402, OperationMode

from bench import Bench

log = logging.getLogger(__name__)

MOTION_DRIVES = ["integro"]

# 0x6041 bits. Bit 13 is the following error in the position modes. Bit 10 is target reached in
# Profile Position.
FOLLOWING_ERROR_BIT = 1 << 13
TARGET_REACHED_BIT = 1 << 10
# 0x6040 for Operation enabled, and the same with bit 4, the new setpoint of Profile Position.
CONTROLWORD_ENABLE_OPERATION = 0x000F
CONTROLWORD_NEW_SETPOINT = 0x001F

# The following error window (0x6065) and the position window (0x6067) of the rollover test, in
# the units of 0x6064. The firmware built target +/- window in 32 bits, which overflowed for a
# target closer to the rollover than the window.
WINDOW = 10_000
# The position the home offset moves the drive to: inside the window below the rollover.
NEAR_ROLLOVER = 2**31 - WINDOW // 2
HOLD_SECONDS = 3.0
# The Profile Position target: closer to the rollover than the window, on the same side.
PP_TARGET = 2**31 - WINDOW // 4
# The slow crossing in CSP: one setpoint step every STEP_SECONDS, STEPS steps of STEP.
STEP = WINDOW // 50
STEPS = 100
STEP_SECONDS = 0.02
TARGET_REACHED_TIMEOUT_SECONDS = 5.0

# The torque offset (0x60B2) of the mode change test, in per mille of the rated torque, as 0x6077.
TORQUE_OFFSET = 100
# Before the fix, the torque stepped by the whole offset after the change from CST to CSP.
MAX_TORQUE_STEP = TORQUE_OFFSET // 2
BEFORE_SWITCH_SECONDS = 0.5
AFTER_SWITCH_SECONDS = 1.0
STEP_WINDOW_US = 300_000


def _int32(value: int) -> int:
    """Return value wrapped to a signed 32-bit integer, as 0x6064 and 0x607A wrap."""
    return (value + 2**31) % 2**32 - 2**31


def _to_operation_enabled_in(bench: Bench, position: int, mode: OperationMode) -> None:
    client = bench.client
    client.reset()
    client.init()
    client.restart(AlState.PREOP)
    client.restart(AlState.OP, positions=[position])
    client.set_operation_mode(position, mode)


@pytest.mark.parametrize("folder", MOTION_DRIVES)
def test_position_control_near_the_rollover(bench: Bench, record_property, folder: str):
    device = bench.device(folder)
    position = device.position
    client = bench.client
    client.reset()
    client.init()
    client.restart(AlState.PREOP)
    saved = {
        obj: client.read_value(position, obj)
        for obj in (Cia402.FOLLOWING_ERROR_WINDOW, Cia402.POSITION_WINDOW, Cia402.HOME_OFFSET)
    }
    log.info("device %d: saved %s", position, saved)
    phase = "setup"
    try:
        # Written at PREOP, because a write of the home offset in OP does not move 0x6064. The
        # drive then goes up to OP without INIT, so that it keeps the written values.
        client.write_value(position, Cia402.FOLLOWING_ERROR_WINDOW, WINDOW)
        client.write_value(position, Cia402.POSITION_WINDOW, WINDOW)
        actual = client.read_value(position, Cia402.POSITION_ACTUAL_VALUE)
        home_offset = _int32(NEAR_ROLLOVER - actual + saved[Cia402.HOME_OFFSET])
        client.write_value(position, Cia402.HOME_OFFSET, home_offset)
        client.ensure_state(AlState.OP, positions=[position])
        client.set_operation_mode(position, OperationMode.CYCLIC_SYNC_POSITION)
        time.sleep(0.2)
        moved = client.read_value(position, Cia402.POSITION_ACTUAL_VALUE)
        assert abs(moved - NEAR_ROLLOVER) < WINDOW // 4, (
            f"0x607C = {home_offset} put 0x6064 at {moved}, not near {NEAR_ROLLOVER}"
        )

        parameters = [(position, *Cia402.STATUSWORD), (position, *Cia402.POSITION_ACTUAL_VALUE)]
        with client.monitoring(parameters, interval_ms=50) as m:
            # Standstill inside the window below the rollover.
            phase = "standstill"
            client.set_target(position, "position", moved)
            client.enable(position)
            time.sleep(HOLD_SECONDS)
            # Profile Position to a target closer to the rollover, on the same side of it, so the
            # move is short.
            phase = "target reached"
            client.set_operation_mode(position, OperationMode.PROFILE_POSITION)
            client.set_target(position, "position", PP_TARGET)
            client.write_value(position, Cia402.CONTROLWORD, CONTROLWORD_NEW_SETPOINT)
            time.sleep(0.1)
            client.write_value(position, Cia402.CONTROLWORD, CONTROLWORD_ENABLE_OPERATION)
            deadline = time.monotonic() + TARGET_REACHED_TIMEOUT_SECONDS
            reached = False
            while time.monotonic() < deadline and not reached:
                time.sleep(0.1)
                reached = bool(
                    client.read_value(position, Cia402.STATUSWORD) & TARGET_REACHED_BIT
                )
            # A slow crossing of the rollover in CSP, one small setpoint step at a time.
            phase = "slow crossing"
            target = client.read_value(position, Cia402.POSITION_ACTUAL_VALUE)
            client.set_target(position, "position", target)
            client.set_operation_mode(position, OperationMode.CYCLIC_SYNC_POSITION)
            for _ in range(STEPS):
                target = _int32(target + STEP)
                client.set_target(position, "position", target)
                time.sleep(STEP_SECONDS)
            time.sleep(0.5)
            crossed_to = client.read_value(position, Cia402.POSITION_ACTUAL_VALUE)
            phase = "end"
        error_code = client.read_value(position, Cia402.ERROR_CODE)
    finally:
        log.info("device %d: the test ended in phase %s", position, phase)
        # A failed step here must not hide the error of the test.
        with contextlib.suppress(Exception):
            client.disable(position)
        for obj, value in saved.items():
            with contextlib.suppress(Exception):
                client.write_value(position, obj, value)
        with contextlib.suppress(Exception):
            client.restart(AlState.INIT, positions=[position])

    following_errors = [
        t for t, sw in m.column(position, Cia402.STATUSWORD) if sw & FOLLOWING_ERROR_BIT
    ]
    faults = [t for t, sw in m.column(position, Cia402.STATUSWORD) if Cia402.state(sw) == "Fault"]
    assert not following_errors, (
        f"0x6041 bit 13 in {len(following_errors)} cycle(s); 0x603F 0x{error_code:04X}"
    )
    assert not faults, f"Fault in {len(faults)} cycle(s); 0x603F 0x{error_code:04X}"
    assert crossed_to < 0, f"0x6064 is {crossed_to} after the crossing, so it did not cross"
    assert reached, f"target reached never set for 0x607A = {PP_TARGET}"
    record_property(
        "message",
        f"device {position}: windows {WINDOW}; held {HOLD_SECONDS} s at {moved}; Profile"
        f" Position to {PP_TARGET} set target reached; crossed the rollover in CSP to"
        f" {crossed_to}; no following error, no fault",
    )


@pytest.mark.parametrize("folder", MOTION_DRIVES)
def test_no_torque_step_from_the_torque_offset_at_a_change_from_cst_to_csp(
    bench: Bench, record_property, folder: str
):
    device = bench.device(folder)
    position = device.position
    client = bench.client
    _to_operation_enabled_in(bench, position, OperationMode.CYCLIC_SYNC_TORQUE)
    saved_offset = client.read_value(position, Cia402.TORQUE_OFFSET)
    try:
        # The target torque cancels the offset, so the torque demand is zero and the free motor
        # does not speed up in CST.
        client.set_target(position, "torque", -TORQUE_OFFSET)
        client.write_value(position, Cia402.TORQUE_OFFSET, TORQUE_OFFSET)
        parameters = [
            (position, *Cia402.TORQUE_ACTUAL_VALUE),
            (position, *Cia402.MODES_OF_OPERATION_DISPLAY),
        ]
        with client.monitoring(parameters, interval_ms=50) as m:
            client.enable(position)
            time.sleep(BEFORE_SWITCH_SECONDS)
            actual = client.read_value(position, Cia402.POSITION_ACTUAL_VALUE)
            client.set_target(position, "position", actual)
            client.set_operation_mode(position, OperationMode.CYCLIC_SYNC_POSITION)
            time.sleep(AFTER_SWITCH_SECONDS)
    finally:
        # A failed step here must not hide the error of the test.
        with contextlib.suppress(Exception):
            client.disable(position)
        with contextlib.suppress(Exception):
            client.write_value(position, Cia402.TORQUE_OFFSET, saved_offset)
        with contextlib.suppress(Exception):
            client.restart(AlState.INIT, positions=[position])

    csp = int(OperationMode.CYCLIC_SYNC_POSITION)
    t_switch = m.first(position, Cia402.MODES_OF_OPERATION_DISPLAY, lambda v: v == csp)
    assert t_switch is not None, "0x6061 never showed CSP"
    before = m.values(position, Cia402.TORQUE_ACTUAL_VALUE, end=t_switch)
    after = m.values(
        position, Cia402.TORQUE_ACTUAL_VALUE, start=t_switch, end=t_switch + STEP_WINDOW_US
    )
    assert before and after, "no torque sample around the change"
    step = max(abs(v) for v in after)
    summary = (
        f"device {position}: 0x60B2 {TORQUE_OFFSET}, CST to CSP; 0x6077 before the change"
        f" {min(before)}..{max(before)}, largest in the {STEP_WINDOW_US // 1000} ms after it {step}"
    )
    log.info("%s", summary)
    assert step < MAX_TORQUE_STEP, summary
    record_property("message", summary)
