"""The objects of the second touch probe channel, which firmware 5.7 adds to SOMANET drives.

Firmware 5.7 adds 0x60BC and 0x60BD, the positions that touch probe 2 captures, and 0x60D3 and
0x60D4, their time stamps. It removes 0x2212 (GPIO input actions), which the touch probe replaces.
The types are those of the ESI of firmware 5.7. The bench has no input wired to a probe, so these
tests check the objects only, not what the probe captures.
"""

from __future__ import annotations

import re

import pytest
from motion_master_client import AlState, Profile

from bench import Bench

# The drives that run SOMANET firmware 5.7 on the bench. The Jasper-based drive runs other firmware.
DRIVES = ["integro", "circulo", "node"]

# ETG.1020 data type codes.
INTEGER32 = 0x0004
UNSIGNED32 = 0x0007

# (index, the name in the ESI, the ETG.1020 data type)
TOUCH_PROBE_2_OBJECTS = [
    (0x60BC, "Touch probe 2 positive edge", INTEGER32),
    (0x60BD, "Touch probe 2 negative edge", INTEGER32),
    (0x60D3, "Touch probe time stamp 2 positive value", UNSIGNED32),
    (0x60D4, "Touch probe time stamp 2 negative value", UNSIGNED32),
]
GPIO_INPUT_ACTIONS = 0x2212
OBJECT_DOES_NOT_EXIST = 0x06020000

_VERSION = re.compile(r"v?(\d+)\.(\d+)")


def _major_minor(version: str) -> tuple[int, int]:
    match = _VERSION.match(version)
    if match is None:
        raise ValueError(f"{version!r} is not a firmware version")
    return int(match.group(1)), int(match.group(2))


@pytest.mark.parametrize("folder", DRIVES)
def test_touch_probe_2_objects_exist_and_0x2212_is_gone(
    bench: Bench, record_property, folder: str
):
    device = bench.device(folder)
    client = bench.client
    client.reset()
    client.init()
    client.restart(AlState.PREOP)
    version = client.read_value(device.position, Profile.MANUFACTURER_SOFTWARE_VERSION)
    if _major_minor(version) < (5, 7):
        pytest.skip(f"device {device.position} runs {version}; the objects come with 5.7")

    dictionary = {(p["index"], p["subindex"]): p for p in client.parameters(device.position)}
    values = []
    for index, name, data_type in TOUCH_PROBE_2_OBJECTS:
        entry = dictionary.get((index, 0))
        assert entry is not None, f"0x{index:04X}:00 ({name}) is not in the dictionary"
        assert entry["dataType"] == data_type, (
            f"0x{index:04X}:00 has data type 0x{entry['dataType']:04X}, the ESI says"
            f" 0x{data_type:04X}"
        )
        response = client.call(
            "sdoUpload",
            path={"slavePosition": device.position, "index": index, "subindex": 0},
            check=False,
        )
        assert response.status_code == 200, f"0x{index:04X}:00: {response.text}"
        data = response.json()["data"]
        assert len(data) == 4, f"0x{index:04X}:00 gave {len(data)} bytes: {data}"
        values.append(f"0x{index:04X} {int.from_bytes(bytes(data), 'little')}")

    removed = sorted(a for a in dictionary if a[0] == GPIO_INPUT_ACTIONS)
    assert not removed, f"the dictionary still holds 0x2212: {removed}"
    response = client.call(
        "sdoUpload",
        path={"slavePosition": device.position, "index": GPIO_INPUT_ACTIONS, "subindex": 0},
        check=False,
    )
    assert response.status_code != 200, f"0x2212:00 answered a value: {response.text}"
    assert f"SDO abort 0x{OBJECT_DOES_NOT_EXIST:08X}" in response.json()["error"], response.text

    record_property(
        "message",
        f"device {device.position} on {version}: {', '.join(values)}, each a 4-byte value of the"
        f" ESI type; 0x2212 not in the dictionary, and its upload aborts 0x06020000",
    )
