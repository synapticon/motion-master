"""The bench under test: its description, its power supply, and the server that drives it."""

from __future__ import annotations

import os
import time
import tomllib
from dataclasses import dataclass
from pathlib import Path

import requests

from motion_master_client import MotionMasterClient

_HERE = Path(__file__).resolve().parent


@dataclass(frozen=True)
class BenchDevice:
    """One device of a bench, and the folder that holds its files."""

    position: int
    product_name: str
    folder: Path

    @property
    def path(self) -> dict[str, int]:
        """The path parameters that address this device in the API, for client.call(path=...)."""
        return {"slavePosition": self.position}

    def file(self, name: str) -> Path:
        """Return the path of a file of the device, by the name it has on the device."""
        return self.folder / name


@dataclass(frozen=True)
class BenchDescription:
    """The contents of bench.toml, with the folder of each device in devices/."""

    adapter: str
    psu_host: str
    psu_off_seconds: float
    boot_timeout_seconds: float
    devices: list[BenchDevice]

    @property
    def product_names(self) -> list[str]:
        return [d.product_name for d in self.devices]

    def device(self, folder: str) -> BenchDevice:
        """Return the device whose files are in devices/<folder>, for example "integro"."""
        for d in self.devices:
            if d.folder.name == folder:
                return d
        raise KeyError(f"bench.toml has no device with folder {folder!r}")

    @classmethod
    def load(cls) -> BenchDescription:
        with open(_HERE / "bench.toml", "rb") as f:
            data = tomllib.load(f)
        return cls(
            adapter=data["adapter"],
            psu_host=data["psu"]["host"],
            psu_off_seconds=data["psu"]["off_seconds"],
            boot_timeout_seconds=data["psu"]["boot_timeout_seconds"],
            devices=[
                BenchDevice(position, d["productName"], _HERE / "devices" / d["folder"])
                for position, d in enumerate(data["devices"], start=1)
            ],
        )


class Psu:
    """The output of the P 1535, switched by the ESP32 firmware in hil/p1535."""

    def __init__(self, host: str):
        self._url = f"http://{host}"

    def switch(self, on: bool) -> None:
        response = requests.get(f"{self._url}/output-{'on' if on else 'off'}", timeout=5)
        response.raise_for_status()


class Bench:
    """The server under test, connected to the bench."""

    def __init__(self, description: BenchDescription, http_url: str, ws_url: str):
        self.description = description
        self.psu = Psu(description.psu_host)
        self.client = MotionMasterClient(
            http_url,
            ws_url=ws_url,
            verify=False,
            fieldbus={"driver": "soem", "adapter": description.adapter},
        )

    @classmethod
    def from_environment(cls) -> Bench:
        """Build the bench from bench.toml, MM_HTTP_URL and MM_WS_URL, after the server answers."""
        description = BenchDescription.load()
        http_url = os.environ.get("MM_HTTP_URL", "https://127.0.0.1:61447")
        ws_url = os.environ.get("MM_WS_URL", "wss://127.0.0.1:62281")
        _wait_for_server(http_url, timeout=60)
        return cls(description, http_url, ws_url)

    def device(self, folder: str) -> BenchDevice:
        """Return the device whose files are in devices/<folder>, for example "integro"."""
        return self.description.device(folder)

    def reset(self) -> None:
        """Close the driver and forget every device, so that the next step starts from nothing."""
        self.client.call("reset")

    def power_cycle(self) -> None:
        """Switch the drives off and on, and return when every one of them answers on the wire.

        The driver is closed first and is open when this returns, with no device scanned.
        """
        self.reset()
        self.switch_off_and_on()
        self.client.init()
        self.wait_for_devices_on_wire()

    def switch_off_and_on(self) -> None:
        """Switch the drives off for long enough that every one loses power, then on again."""
        self.psu.switch(False)
        time.sleep(self.description.psu_off_seconds)
        self.psu.switch(True)

    def wait_for_devices_on_wire(self) -> None:
        """Wait until the wire carries as many devices as the description names. Open the driver first."""
        expected = len(self.description.product_names)
        deadline = time.monotonic() + self.description.boot_timeout_seconds
        on_wire = None
        while time.monotonic() < deadline:
            on_wire = self.client.call("getScanStatus").json()["devicesOnWire"]
            if on_wire == expected:
                return
            time.sleep(1)
        raise TimeoutError(
            f"{on_wire} device(s) on the wire after {self.description.boot_timeout_seconds} s,"
            f" expected {expected}"
        )


def switch_off() -> None:
    """Switch the drives off, with no server. The workflow calls this as its last step."""
    Psu(BenchDescription.load().psu_host).switch(False)


def _wait_for_server(url: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while True:
        try:
            if requests.get(f"{url}/api/version", verify=False, timeout=5).ok:
                return
        except requests.ConnectionError:
            pass
        if time.monotonic() >= deadline:
            raise TimeoutError(f"no answer from {url} after {timeout} s")
        time.sleep(1)
