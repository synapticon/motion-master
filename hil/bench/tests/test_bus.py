"""Open the driver, scan the bus, and bring the devices to PRE-OP."""

from __future__ import annotations

from motion_master_client import PRE_OP

from bench import Bench


def _found(names: list[str]) -> str:
    return f"found {len(names)} devices: {', '.join(names)}"


def test_init_opens_the_driver_once(bench: Bench, record_property):
    bench.reset()
    body = {"driver": "soem", "adapter": bench.description.adapter}

    first = bench.client.call("init", body=body, check=False)
    second = bench.client.call("init", body=body, check=False)

    assert first.status_code == 200, first.text
    assert second.status_code == 409, second.text
    record_property("message", "the first init answered 200, the second 409")


def test_scan_finds_the_devices_in_bus_order(bench: Bench, record_property):
    bench.reset()
    bench.client.init()

    found = bench.client.call("scan").json()["slaves"]

    assert found == len(bench.description.devices)
    names = [d["productName"] for d in bench.client.devices()]
    assert names == [d.product_name for d in bench.description.devices]
    record_property("message", _found(names))


def test_scan_after_a_power_cycle_finds_the_devices_again(bench: Bench, record_property):
    bench.reset()
    bench.client.init()
    bench.client.call("scan")

    bench.switch_off_and_on()
    # The drives restart with no station address, so the last scan is wrong until a new one.
    bench.wait_for_devices_on_wire()
    status = bench.client.call("getScanStatus").json()
    found = bench.client.call("scan").json()["slaves"]

    assert status["required"], status
    assert found == len(bench.description.devices)
    names = [d["productName"] for d in bench.client.devices()]
    assert names == [d.product_name for d in bench.description.devices]
    record_property("message", f"the server asked for a new scan; {_found(names)}")


def test_pre_op_reads_the_object_dictionary_of_every_device(bench: Bench, record_property):
    bench.reset()
    bench.client.init()

    # Through INIT, because a step down to PRE-OP is not the transition this test is about.
    bench.client.restart(PRE_OP)

    states = bench.client.call("getDeviceStates").json()
    assert [d["alState"] for d in states] == [PRE_OP] * len(bench.description.devices)
    counts = []
    for device in bench.description.devices:
        path = {"slavePosition": device.position}
        parameters = bench.client.call("getDeviceParameters", path=path).json()
        assert parameters, f"device {device.position} has no parameters"
        counts.append(f"{device.position}: {len(parameters)}")
    record_property("message", f"{len(states)} devices in PRE-OP; parameters {', '.join(counts)}")
