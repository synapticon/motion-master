"""What each device of the bench says it is, read live at the start of a run.

The report goes to devices.json beside this file, and to the job summary when the run is in GitHub
Actions. Where the device does not match the files of its folder, the report says so, and the run
continues: a swapped drive is a fact the reader needs, and the tests decide what fails.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

from bench import Bench, BenchDevice

REPORT_PATH = Path(__file__).resolve().parent / "devices.json"

# The fields of a hardware description that name one physical product.
_IDENTITY = ("id", "version", "serialNumber")


def write_device_report(bench: Bench) -> list[dict]:
    """Read every device, and write devices.json and the job summary. Call with every device in PRE-OP."""
    live_devices = bench.client.call("getDevices").json()
    entries = []
    for device in bench.description.devices:
        live = live_devices[device.position - 1] if device.position <= len(live_devices) else None
        entries.append(_entry(bench, device, live))
    REPORT_PATH.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")
    server_version = bench.client.call("getVersion").json()["version"]
    append_to_job_summary(_markdown(bench, server_version, entries))
    return entries


def append_to_job_summary(markdown: str) -> None:
    """Add markdown to the job summary of GitHub Actions. Outside GitHub Actions, do nothing."""
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(markdown)


def test_results_markdown(results: list[tuple[str, str, str]]) -> str:
    """Return a table of (test, outcome, message), one row per test, in the order they ran."""
    counts = {}
    for _, outcome, _ in results:
        counts[outcome] = counts.get(outcome, 0) + 1
    lines = [
        "## Test results",
        "",
        ", ".join(f"{count} {outcome}" for outcome, count in counts.items()) or "No test ran.",
        "",
        "| Test | Result | Message |",
        "| --- | --- | --- |",
    ]
    for name, outcome, message in results:
        lines.append(f"| `{name}` | {outcome} | {_cell(message)} |")
    return "\n".join(lines) + "\n\n"


def _cell(text: str) -> str:
    """Make text safe for one cell of a markdown table."""
    return text.replace("|", "\\|").replace("\n", "<br>")


def _entry(bench: Bench, device: BenchDevice, live: dict | None) -> dict:
    entry = {"position": device.position, "folder": device.folder.name, "differences": []}
    if live is None:
        entry["differences"].append("no device at this position")
        return entry

    path = {"slavePosition": device.position}
    entry.update(
        productName=live["productName"],
        vendorId=f"0x{live['vendorId']:08x}",
        productCode=f"0x{live['productCode']:08x}",
        revisionNumber=f"0x{live['revisionNumber']:08x}",
        serialNumber=live["serialNumber"],
    )
    if live["productName"] != device.product_name:
        entry["differences"].append(
            f"productName is {live['productName']!r}, bench.toml says {device.product_name!r}"
        )

    entry["firmwareVersion"] = _read(
        bench, "readParameter", {**path, "index": 0x100A, "subindex": 0}, lambda r: r["value"]
    )
    entry["firmwareDescriptors"] = _read(bench, "getDeviceFirmwareDescriptors", path)
    hardware = _read(bench, "getDeviceHardwareDescription", path)
    entry["hardwareDescription"] = hardware

    stored_path = device.file(".hardware_description")
    if isinstance(hardware, dict) and stored_path.exists():
        stored = json.loads(stored_path.read_text(encoding="utf-8"))
        entry["differences"] += _identity_differences(stored, hardware)
    return entry


def _read(bench: Bench, operation: str, path: dict, pick=lambda r: r):
    """Return the answer of one read, or a line that says why there is none."""
    # An older server, such as a pulled image, can lack the operation. The report then says so,
    # and the tests still run.
    try:
        response = bench.client.call(operation, path=path, check=False)
    except KeyError:
        return f"error: the server has no operation {operation}"
    if not response.ok:
        return f"error: HTTP {response.status_code}, {response.text.strip()}"
    return pick(response.json())


def _identity_differences(stored: dict, live: dict) -> list[str]:
    differences = []
    for part in ("device", "assembly"):
        a, b = stored.get(part) or {}, live.get(part) or {}
        for field in _IDENTITY:
            if a.get(field) != b.get(field):
                differences.append(
                    f"{part}.{field} is {b.get(field)!r}, .hardware_description in the folder"
                    f" says {a.get(field)!r}"
                )
    return differences


def _markdown(bench: Bench, server_version: str, entries: list[dict]) -> str:
    lines = [
        "## Devices of the bench",
        "",
        f"Server version: {server_version}",
        "",
        "| Position | Product | Product code | Revision | Firmware | Build descriptor | Serial number |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for e in entries:
        hardware = e.get("hardwareDescription")
        product = (hardware.get("assembly") or hardware["device"]) if isinstance(hardware, dict) else {}
        lines.append(
            f"| {e['position']} | {e.get('productName', '')} | {e.get('productCode', '')}"
            f" | {e.get('revisionNumber', '')} | {e.get('firmwareVersion', '')}"
            f" | {product.get('buildDescriptor', '')} | {product.get('serialNumber', '')} |"
        )
    differences = [(e["position"], d) for e in entries for d in e["differences"]]
    if differences:
        lines += ["", "**The devices differ from their folders:**", ""]
        lines += [f"- Position {position}: {d}" for position, d in differences]
    failed = [
        (e["position"], field, value)
        for e in entries
        for field, value in e.items()
        if isinstance(value, str) and value.startswith("error:")
    ]
    if failed:
        lines += ["", "**Reads that failed:**", ""]
        lines += [f"- Position {position}, {field}: {value}" for position, field, value in failed]
    return "\n".join(lines) + "\n\n"
