"""Install a firmware package on each device of the bench before the tests.

The packages are not in this repository, because the repository is public. They stay in one folder
on the machine of the bench, which collects the packages of every release. pytest gets the version
to install with --firmware-version, and the folder with --firmware-packages.
"""

from __future__ import annotations

import logging
import time
from collections.abc import Callable
from pathlib import Path

from motion_master_client import AlState, procedure_failure
from motion_master_objects import Profile

from bench import Bench, BenchDevice

log = logging.getLogger(__name__)

PROCEDURE = "firmware-installation"

# The folder of the packages on the machine of the bench.
DEFAULT_FOLDER = Path.home() / "firmware-packages"


def find_package(folder: Path, device: BenchDevice, version: str) -> Path | None:
    """Return the package in folder for the firmwareDescriptor of device and version.

    version is the version in the package name, with or without its leading v, such as
    5.6.11+15.g482835090. Return None when folder holds no such package, and raise
    FileNotFoundError when it holds more than one.
    """
    name_version = f"v{version.removeprefix('v')}"
    matches = sorted(
        folder.glob(f"package_*_{device.firmware_descriptor}_motion-drive_{name_version}.zip")
    )
    if not matches:
        return None
    if len(matches) > 1:
        found = ", ".join(p.name for p in matches)
        raise FileNotFoundError(
            f"{folder} needs exactly one package for {device.firmware_descriptor}"
            f" {name_version}, found {found}"
        )
    return matches[0]


def install_firmware(
    bench: Bench, version: str, folder: Path, record: Callable[[str, object], None]
) -> None:
    """Install version on each device that has a firmwareDescriptor and a package of version, then
    cold-start the bench.

    The devices are installed one after the other, in bus order. After the installs, a power cycle
    starts every drive from the new firmware, and each device must then report the version of its
    package in object 0x100A. record is pytest's record_testsuite_property.

    Raise RuntimeError when an install fails or a device reports another version. The tests after
    it would test the wrong firmware.
    """
    candidates = [d for d in bench.description.devices if d.firmware_descriptor]
    packages = {d.position: find_package(folder, d, version) for d in candidates}
    # A release can have packages for some devices only, as an older release kept for a
    # comparison does. Such a device keeps its firmware. A version with no package at all is a
    # mistake, because then nothing would be installed.
    devices = [d for d in candidates if packages[d.position] is not None]
    for device in candidates:
        if packages[device.position] is None:
            log.info(
                "device %d: no package for %s, it keeps its firmware", device.position, version
            )
            record(f"firmware.{device.position}.package", "none, the firmware was kept")
    if not devices:
        raise FileNotFoundError(f"{folder} holds no package for {version}")

    bench.client.ensure_scanned()
    for device in devices:
        package = packages[device.position]
        start = time.monotonic()
        log.info("device %d: installing %s", device.position, package.name)
        bench.client.write_user_cache_file(f"firmwares/{package.name}", package.read_bytes())
        body = {"packageFilename": package.name, "finalState": AlState.PREOP}
        last = None
        for snapshot in bench.client.run_procedure(device.position, PROCEDURE, body):
            last = snapshot
        failure = procedure_failure(last)
        record(f"firmware.{device.position}.package", package.name)
        record(f"firmware.{device.position}.seconds", round(time.monotonic() - start, 3))
        if failure is not None:
            raise RuntimeError(f"device {device.position}: {PROCEDURE} failed: {failure}")

    # A package can write an SII, which applies only after a power cycle.
    bench.cold_start()
    bench.client.ensure_state(AlState.PREOP, exact=True)
    expected = f"v{version.removeprefix('v')}"
    for device in devices:
        actual = bench.client.read_value(device.position, Profile.MANUFACTURER_SOFTWARE_VERSION)
        if actual != expected:
            raise RuntimeError(
                f"device {device.position} reports firmware {actual}, expected {expected}"
            )
        log.info("device %d: runs %s", device.position, actual)
