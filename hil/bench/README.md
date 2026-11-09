# hil/bench: tests on the drives of a bench

These tests run against a Motion Master server that is connected to real drives. A bench is a
computer with an EtherCAT bus, the drives on that bus, and a PeakTech P 1535 power supply that
supplies the drives. The tests switch the output of the power supply through the ESP32 firmware in
[`hil/p1535`](../p1535), so a test can start from drives that just booted.

The tests use the Python reference client in [`clients/python`](../../clients/python). So a run
also checks that client.

## The bench description

This folder describes one bench: the bench of the self-hosted runner `ci-belgrade-linux`.

`bench.toml` holds the settings of the bench:

- `adapter` is the network interface of the EtherCAT bus.
- `[psu]` gives the address of the P 1535, the time the output stays off in a power cycle, and the
  time the drives get to boot.
- `[[devices]]` lists the devices in bus order. `productName` is the value that
  `GET /api/devices` reports. `folder` is the name of the device's subfolder in `devices/`, for
  example `2-circulo`.

The folder of a device holds the files of that device, with the names they have on the device:
`config.csv`, `.hardware_description`, `.factory_config` and others. A file is a known-good copy.
A test can compare the device with it, or write it back to the device.

## The device report

Before the first test, the run brings every device to PRE-OP and reads what it says it is: the
product code, the revision, the firmware version from object `0x100A`, and the build descriptor and
the serial number from the hardware description. In GitHub Actions the run writes this as a table
to the job summary.

Where a device does not match its folder, the report names the difference. The run does not fail
for it. The report compares the product name, and the id, the version and the serial number in
`.hardware_description`.

## The report file

With `--junitxml=report.xml`, pytest writes a JUnit XML report. JUnit XML is the test report format
that most CI tools read. A passed test case carries a `message` property with what the test saw. A
failed one carries the failure and its traceback, as JUnit XML always does.

The device report goes into the same file, as properties of the test suite, so that one file holds
everything about a run:

| Property | Value |
| --- | --- |
| `server.version` | The version of the server under test |
| `setup.seconds` | How long the start of the run took: the wait for the server, the power cycle and the init. JUnit counts this time in the time of the first test |
| `report.seconds` | How long the device report took, with the transition to PRE-OP that its reads need. JUnit counts this time in the time of the first test too |
| `device.<position>.folder` | The folder of the device in `devices/` |
| `device.<position>.productName` | The product name that `GET /api/devices` reports |
| `device.<position>.productCode` | The product code, in hexadecimal |
| `device.<position>.revisionNumber` | The revision number, in hexadecimal |
| `device.<position>.firmwareVersion` | The value of object `0x100A` |
| `device.<position>.buildDescriptor` | The build descriptor from the hardware description |
| `device.<position>.serialNumber` | The serial number from the hardware description |
| `device.<position>.differences` | Where the device differs from its folder, separated by `;` |
| `device.<position>.failedReads` | The reads that failed, separated by `;` |
| `report.error` | Only when the device report failed: why |

A field that the device did not give is empty. Every device has every property, so a reader can
rely on the keys.

## Run the tests

A run starts with one power cycle. Every test then starts with the driver closed. A test does not
leave the bus as it found it. When the last test ends, passed or not, the run switches the drives
off.

1. Start the server, and connect the bench to it.
2. Install the dependencies.

   ```bash
   cd hil/bench
   python3 -m venv .venv
   source .venv/bin/activate
   pip install -r requirements.txt
   ```

3. Run all tests.

   ```bash
   pytest
   ```

4. To run some tests only, give a pytest `-k` expression.

   ```bash
   pytest -k "scan and not power"
   ```

Two environment variables select the server:

| Variable | Default | Description |
| --- | --- | --- |
| `MM_HTTP_URL` | `https://127.0.0.1:61447` | The HTTP API of the server under test |
| `MM_WS_URL` | `wss://127.0.0.1:62281` | The WebSocket of the server under test |

The tests reset the server and switch the drives off. Do not run them against a server that
somebody else uses.

## Run the tests in GitHub Actions

The workflow **Bench Tests** (`.github/workflows/bench-tests.yml`) runs the tests on
`ci-belgrade-linux`. Start it by hand from the Actions tab. It has two inputs:

| Input | Empty | Set |
| --- | --- | --- |
| `version` | Build the Docker image of the server from the selected branch. | Pull `synapticon/motion-master:<version>` from Docker Hub and test that image. A version that is not on Docker Hub fails the run before any test runs. |
| `tests` | Run every test. | Run the tests that a pytest `-k` expression selects, for example `scan and not power`. |

The tests always come from the selected branch, also when the server is a pulled image. The job
summary names the server that ran. The workflow starts the server with the capabilities that SOEM
needs. When the tests end, it uploads two files: `report.xml`, the report file, and `server.log`,
the log of the server.

The run page shows two tables below the job graph, in the job summary: the device report, and one
row for each test with its result and its message.

Only one run uses the bench at a time. A second run waits.
