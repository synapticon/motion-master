"""Turn the motor of a drive in Profile Velocity, quick stop it, and check the recorded velocity.

Each device needs its own target velocity, limits and times, so each device gets its own test.
Only the Integro has one.
"""

from __future__ import annotations

import contextlib
import logging
import time

import pytest
from motion_master_client import AlState, Cia402

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

MEAN_TOLERANCE = 0.01
SAMPLE_TOLERANCE = 0.02
STOPPED_RPM = 20
STOP_DEADLINE_US = 300_000

# The state a drive ends in after a quick stop, per quick stop option code. The other codes do not
# stop on the quick stop ramp of 0x6085: 0 lets the motor coast, 9 shorts the phases.
END_STATE_AFTER_QUICK_STOP = {2: "SwitchOnDisabled", 6: "QuickStopActive"}


@pytest.mark.usefixtures("process_data_dump")
def test_integro_profile_velocity_reaches_the_target_and_quick_stops(
    bench: Bench, record_property
):
    integro = bench.device("integro")
    client = bench.client

    # A test before may have left a drive in OP, so every drive restarts. Every drive goes to
    # PREOP, because the map of the process data reads the PDO mapping of every slave, and a slave
    # in INIT does not answer (#208).
    bench.client.reset()
    client.init()
    client.restart(AlState.PREOP)
    client.restart(AlState.OP, positions=[integro.position])

    acceleration = client.read_value(integro.position, Cia402.PROFILE_ACCELERATION)
    deceleration = client.read_value(integro.position, Cia402.QUICK_STOP_DECELERATION)
    option_code = client.read_value(integro.position, Cia402.QUICK_STOP_OPTION_CODE)
    settings = f"0x6083 {acceleration} rpm/s, 0x6085 {deceleration} rpm/s, 0x605A {option_code}"
    log.info("%s", settings)
    end_state = END_STATE_AFTER_QUICK_STOP.get(option_code)
    if end_state is None:
        pytest.fail(
            f"0x605A is {option_code}; the test needs 2 or 6, which stop on the 0x6085 ramp"
        )

    parameters = [
        (integro.position, *Cia402.VELOCITY_ACTUAL_VALUE),
        (integro.position, *Cia402.TARGET_VELOCITY),
        (integro.position, *Cia402.STATUSWORD),
    ]
    try:
        with client.monitoring(parameters, interval_ms=50) as monitoring:
            # An object that is not in the process data gives one value per flush, not per cycle.
            assert monitoring.sources == ["pdo"] * len(parameters), monitoring.parameters

            client.set_operation_mode(integro.position, PROFILE_VELOCITY)
            # A stale target velocity would start the motor at the enable.
            client.set_target(integro.position, "velocity", 0)
            client.enable(integro.position)
            client.set_target(integro.position, "velocity", TARGET_RPM)
            time.sleep(HOLD_SECONDS)
            client.quick_stop(integro.position)
            time.sleep(AFTER_STOP_SECONDS)
    finally:
        # A failed step here must not hide the error of the test.
        with contextlib.suppress(Exception):
            client.disable(integro.position)
        with contextlib.suppress(Exception):
            client.restart(AlState.INIT, positions=[integro.position])

    m = monitoring
    position = integro.position

    def is_fault(sw):
        return Cia402.state(sw) == "Fault"

    def left_operation_enabled(sw):
        return Cia402.state(sw) != "OperationEnabled"

    def near_target(v):
        return abs(v - TARGET_RPM) <= TARGET_RPM * MEAN_TOLERANCE

    def stopped(v):
        return abs(v) < STOPPED_RPM

    # 1. The drive exchanged process data in every recorded cycle.
    assert all(None not in row[1:] for row in m.rows), "a row has no value"

    # 2. No fault in the whole recording.
    faults = [sw for sw in m.values(position, Cia402.STATUSWORD) if is_fault(sw)]
    assert not faults, f"the drive was in Fault in {len(faults)} cycle(s)"

    t_target = m.first(position, Cia402.TARGET_VELOCITY, lambda v: v == TARGET_RPM)
    assert t_target is not None, f"0x60FF never read {TARGET_RPM}"
    t_stop = m.first(position, Cia402.STATUSWORD, left_operation_enabled, after=t_target)
    assert t_stop is not None, "the drive never left Operation Enabled after the target"
    assert t_stop - t_target >= WINDOW_US, (
        f"the quick stop came {(t_stop - t_target) / 1000:.0f} ms after the target"
    )

    t_reached = m.first(position, Cia402.VELOCITY_ACTUAL_VALUE, near_target, after=t_target)
    ramp = f"{(t_reached - t_target) / 1000:.0f} ms" if t_reached is not None else "never"

    window = m.values(position, Cia402.VELOCITY_ACTUAL_VALUE, start=t_stop - WINDOW_US, end=t_stop)
    assert window, "no sample in the window before the quick stop"
    mean = sum(window) / len(window)
    summary = (
        f"{settings}; ramp to {TARGET_RPM} rpm {ramp}; last 1 s before the quick stop: "
        f"mean {mean:.1f}, min {min(window)}, max {max(window)} rpm over {len(window)} cycles"
    )
    log.info("%s", summary)

    # 3. The mean of the window is at the target.
    assert near_target(mean), summary
    # 4. Every sample of the window is near the target.
    outside = [v for v in window if abs(v - TARGET_RPM) > TARGET_RPM * SAMPLE_TOLERANCE]
    assert not outside, f"{len(outside)} sample(s) outside ±{SAMPLE_TOLERANCE:.0%}; {summary}"

    # 5. The quick stop brings the motor to a stop in time.
    t_stopped = m.first(position, Cia402.VELOCITY_ACTUAL_VALUE, stopped, after=t_stop)
    assert t_stopped is not None, f"the velocity never fell below {STOPPED_RPM} rpm; {summary}"
    stop_time_us = t_stopped - t_stop
    summary += f"; stopped in {stop_time_us / 1000:.0f} ms"
    assert stop_time_us <= STOP_DEADLINE_US, summary

    # 6. The drive ends in the state that 0x605A names.
    last_state = Cia402.state(m.values(position, Cia402.STATUSWORD)[-1])
    assert last_state == end_state, f"the drive ended in {last_state}, not {end_state}; {summary}"

    record_property("message", summary)
