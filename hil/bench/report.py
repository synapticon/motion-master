"""What each device of the bench says it is, read live at the start of a run.

The report goes to report.xml as test-suite properties, and to the job summary when the run is in
GitHub Actions. Where the device does not match the files of its folder, the report says so, and the
run continues: a swapped drive is a fact the reader needs, and the tests decide what fails.
"""

from __future__ import annotations

import json
import os
from collections.abc import Callable

from bench import Bench, BenchDevice

# The fields of a hardware description that name one physical product.
_IDENTITY = ("id", "version", "serialNumber")

# The fields of a device row that become test-suite properties, as device.<position>.<field>.
_PROPERTIES = (
    "folder",
    "productName",
    "productCode",
    "revisionNumber",
    "firmwareVersion",
    "buildDescriptor",
    "serialNumber",
    "differences",
    "failedReads",
)


def write_device_report(bench: Bench, record: Callable[[str, object], None]) -> list[dict]:
    """Read every device, record it, and write the job summary. Call with every device in PRE-OP.

    record is pytest's record_testsuite_property, which puts each property into report.xml.
    """
    server_version = bench.client.call("getVersion").json()["version"]
    live_devices = bench.client.call("getDevices").json()
    rows = []
    for device in bench.description.devices:
        live = live_devices[device.position - 1] if device.position <= len(live_devices) else None
        rows.append(_row(bench, device, live))

    record("server.version", server_version)
    for row in rows:
        for field in _PROPERTIES:
            value = row[field]
            if isinstance(value, list):
                value = "; ".join(value)
            record(f"device.{row['position']}.{field}", value)
    append_to_job_summary(_markdown(server_version, rows))
    return rows


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


def _row(bench: Bench, device: BenchDevice, live: dict | None) -> dict:
    """Return what one device says it is. A field the device did not give is an empty string."""
    row = {field: "" for field in _PROPERTIES}
    row.update(position=device.position, folder=device.folder.name, differences=[], failedReads=[])
    if live is None:
        row["differences"].append("no device at this position")
        return row

    path = {"slavePosition": device.position}
    row.update(
        productName=live["productName"],
        productCode=f"0x{live['productCode']:08x}",
        revisionNumber=f"0x{live['revisionNumber']:08x}",
    )
    if live["productName"] != device.product_name:
        row["differences"].append(
            f"productName is {live['productName']!r},"
            f" bench.description.toml says {device.product_name!r}"
        )

    firmware, error = _read(bench, "readParameter", {**path, "index": 0x100A, "subindex": 0})
    if error:
        row["failedReads"].append(f"firmware version: {error}")
    else:
        row["firmwareVersion"] = firmware["value"]

    hardware, error = _read(bench, "getDeviceHardwareDescription", path)
    if error:
        row["failedReads"].append(f"hardware description: {error}")
        return row
    # The assembly is the product when there is one, such as a drive with its motor.
    product = hardware.get("assembly") or hardware.get("device") or {}
    row.update(
        buildDescriptor=product.get("buildDescriptor", ""),
        serialNumber=product.get("serialNumber", ""),
    )
    stored_path = device.file(".hardware_description")
    if stored_path.exists():
        stored = json.loads(stored_path.read_text(encoding="utf-8"))
        row["differences"] += _identity_differences(stored, hardware)
    return row


def _read(bench: Bench, operation: str, path: dict) -> tuple[dict, str]:
    """Return the answer of one read and "", or {} and a line that says why there is no answer."""
    # An older server, such as a pulled image, can lack the operation. The report then says so,
    # and the tests still run.
    try:
        response = bench.client.call(operation, path=path, check=False)
    except KeyError:
        return {}, f"the server has no operation {operation}"
    if not response.ok:
        return {}, f"HTTP {response.status_code}, {response.text.strip()}"
    return response.json(), ""


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


def _markdown(server_version: str, rows: list[dict]) -> str:
    lines = [
        "## Devices of the bench",
        "",
        f"Server version: {server_version}",
        "",
        "| Position | Product | Product code | Revision | Firmware | Build descriptor | Serial number |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for r in rows:
        lines.append(
            f"| {r['position']} | {r['productName']} | {r['productCode']} | {r['revisionNumber']}"
            f" | {r['firmwareVersion']} | {r['buildDescriptor']} | {r['serialNumber']} |"
        )
    differences = [(r["position"], d) for r in rows for d in r["differences"]]
    if differences:
        lines += ["", "**The devices differ from their folders:**", ""]
        lines += [f"- Position {position}: {d}" for position, d in differences]
    failed = [(r["position"], f) for r in rows for f in r["failedReads"]]
    if failed:
        lines += ["", "**Reads that failed:**", ""]
        lines += [f"- Position {position}, {f}" for position, f in failed]
    return "\n".join(lines) + "\n\n"
