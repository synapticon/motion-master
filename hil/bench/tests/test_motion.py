"""Turn the motor of a drive in Profile Velocity, quick stop it, and check the recorded velocity.

Each test is for one device, because devices differ in their inputs and in what to expect.
"""

from __future__ import annotations

import logging
import time

import pytest
from motion_master_client import (
    INIT,
    MODES_OF_OPERATION_DISPLAY,
    OP,
    PROFILE_ACCELERATION,
    QUICK_STOP_DECELERATION,
    QUICK_STOP_OPTION_CODE,
    STATUSWORD,
    TARGET_VELOCITY,
    VELOCITY_ACTUAL_VALUE,
    cia402_state,
)

from bench import Bench

log = logging.getLogger(__name__)

PROFILE_VELOCITY = 3
# The inputs and the expectations of the Integro test. A test of another device has its own.
TARGET_RPM = 1000
# The move holds the target this long. The last WINDOW_US of it is the window the checks read, so
# the ramp must end in the time before the window.
HOLD_SECONDS = 3.0
WINDOW_US = 1_000_000
# The recording continues this long after the quick stop, so that it holds the whole stop.
AFTER_STOP_SECONDS = 1.0

MEAN_TOLERANCE = 0.02
SAMPLE_TOLERANCE = 0.10
STOPPED_RPM = 20
STOP_DEADLINE_US = 500_000

# The state a drive ends in after a quick stop, per quick stop option code. The other codes do not
# stop on the quick stop ramp of 0x6085: 0 lets the motor coast, 9 shorts the phases.
END_STATE_AFTER_QUICK_STOP = {2: "SwitchOnDisabled", 6: "QuickStopActive"}


def test_integro_profile_velocity_reaches_the_target_and_quick_stops(
    bench: Bench, record_property
):
    integro = bench.device("integro")
    client = bench.client

    # Every drive in INIT first. A test before may have left a drive in OP, and the server does not
    # command an AL state when it resets.
    bench.reset()
    client.init()
    client.ensure_state(INIT, exact=True)
    client.ensure_state(OP, positions=[integro.position])

    acceleration = client.read_value(integro.position, PROFILE_ACCELERATION)
    deceleration = client.read_value(integro.position, QUICK_STOP_DECELERATION)
    option_code = client.read_value(integro.position, QUICK_STOP_OPTION_CODE)
    settings = f"0x6083 {acceleration} rpm/s, 0x6085 {deceleration} rpm/s, 0x605A {option_code}"
    log.info("%s", settings)
    end_state = END_STATE_AFTER_QUICK_STOP.get(option_code)
    if end_state is None:
        pytest.fail(
            f"0x605A is {option_code}; the test needs 2 or 6, which stop on the 0x6085 ramp"
        )

    parameters = [
        (integro.position, *VELOCITY_ACTUAL_VALUE),
        (integro.position, *TARGET_VELOCITY),
        (integro.position, *STATUSWORD),
    ]
    try:
        with client.monitoring(parameters, interval_ms=50) as monitoring:
            # An object that is not in the process data gives one value per flush, not per cycle.
            assert monitoring.sources == ["pdo"] * len(parameters), monitoring.parameters

            mode = {"mode": PROFILE_VELOCITY}
            client.call("setCia402OperationMode", path=integro.path, body=mode)
            # The drive takes a new mode some cycles after the write.
            client.wait_for_value(
                integro.position,
                MODES_OF_OPERATION_DISPLAY,
                lambda active: active == PROFILE_VELOCITY,
            )
            # A stale target velocity would start the motor at the enable.
            stand_still = {"target": "velocity", "value": 0}
            client.call("setCia402Target", path=integro.path, body=stand_still)
            enabled = {"state": "OperationEnabled"}
            client.call("transitionToCia402State", path=integro.path, body=enabled)
            target_velocity = {"target": "velocity", "value": TARGET_RPM}
            client.call("setCia402Target", path=integro.path, body=target_velocity)
            time.sleep(HOLD_SECONDS)
            client.call("runCia402Command", path=integro.path, body={"command": "quickStop"})
            time.sleep(AFTER_STOP_SECONDS)
    finally:
        # check=False, so that a failed step here does not hide the error of the test.
        client.call("runCia402Command", path=integro.path, body={"command": "disable"}, check=False)
        client.call(
            "transitionToState", body={"state": INIT, "positions": [integro.position]}, check=False
        )

    velocity = monitoring.column(integro.position, *VELOCITY_ACTUAL_VALUE)
    target = monitoring.column(integro.position, *TARGET_VELOCITY)
    statusword = monitoring.column(integro.position, *STATUSWORD)

    # 1. The drive exchanged process data in every recorded cycle.
    assert all(None not in row[1:] for row in monitoring.rows), "a row has no value"

    states = [(t, cia402_state(sw)) for t, sw in statusword]
    faults = [t for t, state in states if state == "Fault"]
    # 2. No fault in the whole recording.
    assert not faults, f"the drive was in Fault in {len(faults)} cycle(s)"

    t_target = next((t for t, value in target if value == TARGET_RPM), None)
    assert t_target is not None, f"0x60FF never read {TARGET_RPM}"
    t_stop = next(
        (t for t, state in states if t > t_target and state != "OperationEnabled"), None
    )
    assert t_stop is not None, "the drive never left Operation Enabled after the target"
    assert t_stop - t_target >= WINDOW_US, (
        f"the quick stop came {(t_stop - t_target) / 1000:.0f} ms after the target"
    )

    tolerance = TARGET_RPM * MEAN_TOLERANCE
    reached = next(
        (t for t, v in velocity if t >= t_target and abs(v - TARGET_RPM) <= tolerance), None
    )
    ramp = f"{(reached - t_target) / 1000:.0f} ms" if reached is not None else "never"

    window = [v for t, v in velocity if t_stop - WINDOW_US <= t < t_stop]
    assert window, "no sample in the window before the quick stop"
    mean = sum(window) / len(window)
    summary = (
        f"{settings}; ramp to {TARGET_RPM} rpm {ramp}; last 1 s before the quick stop: "
        f"mean {mean:.1f}, min {min(window)}, max {max(window)} rpm over {len(window)} cycles"
    )
    log.info("%s", summary)

    # 3. The mean of the window is at the target.
    assert abs(mean - TARGET_RPM) <= tolerance, summary
    # 4. Every sample of the window is near the target.
    outside = [v for v in window if abs(v - TARGET_RPM) > TARGET_RPM * SAMPLE_TOLERANCE]
    assert not outside, f"{len(outside)} sample(s) outside ±{SAMPLE_TOLERANCE:.0%}; {summary}"

    # 5. The quick stop brings the motor to a stop in time.
    t_stopped = next((t for t, v in velocity if t >= t_stop and abs(v) < STOPPED_RPM), None)
    assert t_stopped is not None, f"the velocity never fell below {STOPPED_RPM} rpm; {summary}"
    stop_time_us = t_stopped - t_stop
    summary += f"; stopped in {stop_time_us / 1000:.0f} ms"
    assert stop_time_us <= STOP_DEADLINE_US, summary

    # 6. The drive ends in the state that 0x605A names.
    last_state = states[-1][1]
    assert last_state == end_state, f"the drive ended in {last_state}, not {end_state}; {summary}"

    record_property("message", summary)
