"""Open the driver, scan the bus, and bring the devices to PRE-OP."""

from __future__ import annotations

from motion_master_client import PRE_OP

from bench import Bench


def product_names(bench: Bench) -> list[str]:
    return [d["productName"] for d in bench.client.call("getDevices").json()]


def test_init_opens_the_driver_once(fresh: Bench):
    body = {"driver": "soem", "adapter": fresh.description.adapter}

    first = fresh.client.call("init", body=body, check=False)
    second = fresh.client.call("init", body=body, check=False)

    assert first.status_code == 200, first.text
    assert second.status_code == 409, second.text


def test_scan_finds_the_devices_in_bus_order(fresh: Bench):
    fresh.client.init()

    found = fresh.client.call("scan").json()["slaves"]

    assert found == len(fresh.description.product_names)
    assert product_names(fresh) == fresh.description.product_names


def test_scan_after_a_power_cycle_finds_the_devices_again(fresh: Bench):
    fresh.client.init()
    fresh.client.call("scan")

    fresh.switch_off_and_on()
    # The drives restart with no station address, so the last scan is wrong until a new one.
    fresh.wait_for_devices_on_wire()
    status = fresh.client.call("getScanStatus").json()
    found = fresh.client.call("scan").json()["slaves"]

    assert status["required"], status
    assert found == len(fresh.description.product_names)
    assert product_names(fresh) == fresh.description.product_names


def test_pre_op_reads_the_object_dictionary_of_every_device(fresh: Bench):
    fresh.client.init()

    fresh.client.ensure_state(PRE_OP, exact=True)

    states = fresh.client.call("getDeviceStates").json()
    assert [d["alState"] for d in states] == [PRE_OP] * len(fresh.description.product_names)
    for position in range(1, len(fresh.description.product_names) + 1):
        parameters = fresh.client.call("getDeviceParameters", path={"slavePosition": position})
        assert parameters.json(), f"device {position} has no parameters"
