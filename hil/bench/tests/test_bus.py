"""Open the driver, scan the bus, and change the AL state of the devices."""

from __future__ import annotations

import time

from motion_master_client import INIT, OP, PREOP, SAFEOP, STATE_NAMES

from bench import Bench


def _found(names: list[str]) -> str:
    return f"found {len(names)} devices: {', '.join(names)}"


def test_init_opens_the_driver_once(bench: Bench, record_property):
    bench.client.reset()
    body = {"driver": "soem", "adapter": bench.description.adapter}

    first = bench.client.call("init", body=body, check=False)
    second = bench.client.call("init", body=body, check=False)

    assert first.status_code == 200, first.text
    assert second.status_code == 409, second.text
    record_property("message", "the first init answered 200, the second 409")


def test_scan_finds_the_devices_in_bus_order(bench: Bench, record_property):
    bench.client.reset()
    bench.client.init()

    found = bench.client.scan()

    assert found == len(bench.description.devices)
    names = [d["productName"] for d in bench.client.devices()]
    assert names == [d.product_name for d in bench.description.devices]
    record_property("message", _found(names))


def test_scan_after_a_power_cycle_finds_the_devices_again(bench: Bench, record_property):
    bench.client.reset()
    bench.client.init()
    bench.client.scan()

    bench.psu.cycle()
    # The drives restart with no station address, so the last scan is wrong until a new one.
    bench.wait_for_devices_on_wire()
    status = bench.client.call("getScanStatus").json()
    found = bench.client.scan()

    assert status["required"], status
    assert found == len(bench.description.devices)
    names = [d["productName"] for d in bench.client.devices()]
    assert names == [d.product_name for d in bench.description.devices]
    record_property("message", f"the server asked for a new scan; {_found(names)}")


def test_pre_op_reads_the_object_dictionary_of_every_device(bench: Bench, record_property):
    bench.client.reset()
    bench.client.init()

    # Through INIT, because a step down to PREOP is not the transition this test is about.
    bench.client.restart(PREOP)

    states = bench.client.call("getDeviceStates").json()
    assert [d["alState"] for d in states] == [PREOP] * len(bench.description.devices)
    counts = []
    for device in bench.description.devices:
        parameters = bench.client.parameters(device.position)
        assert parameters, f"device {device.position} has no parameters"
        counts.append(f"{device.position}: {len(parameters)}")
    record_property("message", f"{len(states)} devices in PREOP; parameters {', '.join(counts)}")


def _al_states(bench: Bench) -> list[int]:
    return [d["alState"] for d in bench.client.call("getDeviceStates").json()]


def test_transition_walks_every_device_from_init_to_safe_op(bench: Bench, record_property):
    bench.client.reset()
    bench.client.init()
    bench.client.restart(INIT)

    # One request. The server walks every device through PREOP to SAFEOP.
    start = time.monotonic()
    result = bench.client.call("transitionToState", body={"state": SAFEOP}).json()
    seconds = time.monotonic() - start

    assert result["ok"], result
    assert _al_states(bench) == [SAFEOP] * len(bench.description.devices)
    record_property(
        "message", f"{len(result['devices'])} devices from INIT to SAFEOP in {seconds:.1f} s"
    )


def test_a_single_step_refuses_an_illegal_transition(bench: Bench, record_property):
    bench.client.reset()
    bench.client.init()
    bench.client.restart(INIT)

    # INIT to OP is three transitions, so the single step refuses it and commands nothing.
    illegal = bench.client.call("changeState", body={"state": OP}, check=False)
    after_illegal = _al_states(bench)
    legal = bench.client.call("changeState", body={"state": PREOP}, check=False)

    assert illegal.status_code == 409, illegal.text
    assert after_illegal == [INIT] * len(bench.description.devices)
    assert legal.status_code == 200, legal.text
    assert legal.json()["ok"], legal.json()
    record_property(
        "message",
        f"INIT to {STATE_NAMES[OP]} answered 409 and moved no device;"
        f" INIT to {STATE_NAMES[PREOP]} answered 200",
    )

