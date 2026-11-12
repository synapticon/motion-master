# hil/bench: tests on the drives of a bench

These tests run against a Motion Master server that is connected to real drives. A bench is a
computer with an EtherCAT bus, the drives on that bus, and a PeakTech P 1535 power supply that
supplies the drives. The tests switch the output of the power supply through the ESP32 firmware in
[`hil/p1535`](../p1535), so a test can start from drives that just booted.

The tests use the Python reference client in [`clients/python`](../../clients/python). So a run
also checks that client.

The recording below shows a run. The steps are in [Run the tests](#run-the-tests).
[VHS](https://github.com/charmbracelet/vhs) records it from `demo.tape`. To record it again, run
`vhs demo.tape` on the computer of the bench.

![A run of the bench tests](demo.gif)

## The bench description

This folder describes one bench: the bench of the self-hosted runner `ci-belgrade-linux`.

`bench.description.toml` describes the bench. Every value must match the hardware:

- `adapter` is the network interface of the EtherCAT bus.
- `[psu]` gives the address of the P 1535, the time the output stays off in a power cycle, and the
  time the drives get to boot.
- `[[devices]]` lists the devices in bus order. `productName` is the value that
  `GET /api/devices` reports. `folder` is the name of the device's subfolder in `devices/`, for
  example `circulo`.

The folder of a device holds the files of that device, with the names they have on the device:
`config.csv`, `.hardware_description`, `.factory_config` and others. A file is a known-good copy.
A test can compare the device with it, or write it back to the device.

## The start of each test

The run has one bench, and the tests use it one after the other. Each test brings the bench to the
start it needs with its first lines: `bench.client.reset()` for no state on the server,
`bench.psu.cycle()` for drives that just booted, followed by `bench.client.init()` and
`bench.wait_for_devices_on_wire()`, and `bench.client.restart(AlState.PREOP)` for every drive restarted
through INIT to PREOP. A reset of the server
does not send the drives to any AL state. So a drive keeps the state that the test before left it
in, or drops to SAFEOP with an error when its watchdog trips. A test that needs a known state
requests it.

## The motion test

`tests/test_motion.py` turns the motor of a device in Profile Velocity, the operation mode in which
the drive ramps to a target velocity. Each device needs its own target velocity, limits and times,
so each device gets its own test. Only the Integro has one. The test of the Integro finds it in
`bench.description.toml` by its folder, `integro`. The test restarts every drive to PREOP, then
restarts only the Integro to OP, enables it, holds 1000 rpm for 3 s, and sends a quick stop. A
monitoring records the velocity actual value (`0x606C`), the target velocity (`0x60FF`) and the
statusword (`0x6041`) in every cycle. The test then checks the recorded rows:

- The mean velocity in the last second before the quick stop is within 1 % of the target, and every
  sample is within 2 %.
- The velocity falls below 20 rpm within 300 ms after the quick stop.
- The drive ends in the state that the quick stop option code (`0x605A`) names: Switch On Disabled
  for 2, Quick Stop Active for 6. Other codes fail the test before the motor turns.
- The drive is never in Fault, and every cycle has a value.

The message of the test gives the acceleration and the quick stop deceleration read from the drive,
the ramp time, the mean, the minimum and the maximum velocity, and the stop time. When the test
ends, passed or not, it disables the drive and brings it to INIT.

## Process-data dumps

The server records the process data of every cycle in its recorder ring. With
`pytest --process-data-dumps`, a test marked with `@pytest.mark.usefixtures("process_data_dump")`
gets that ring as a file when it ends, also when it fails: `dumps/<test name>.mmpd`. The option is
off by default, because a dump is large and is needed only to look at the motion data of a failing
test. A `.mmpd` file holds the raw input and output images of every cycle,
with a timestamp, and the layout of the process image, so every PDO object of every exchanging
device can be read from it. Open it in the Recorder page of the Console.

Each test resets the server first, and a reset frees the ring, so a dump holds one test only. A
test that exchanged no process data gets no file. A re-map that changes the layout of the process
image starts a new ring, so a dump holds only the cycles after the last such re-map.

The motion test asks for a dump. In GitHub Actions, the input `process-data-dumps` of the workflow
turns the option on, and the workflow uploads every dump with `report.xml`.

## The bench report

Before the first test, the run reads the version of the server. Then it brings every device to
PREOP and reads what the device says it is: the product code, the revision, the firmware version
from object `0x100A`, and the build descriptor and the serial number from the hardware description.
In GitHub Actions the run writes this as a table to the job summary.

Where a device does not match its folder, the report names the difference. The run does not fail
for it. The report compares the product name, and the id, the version and the serial number in
`.hardware_description`.

## The report file

With `--junitxml=report.xml`, pytest writes a JUnit XML report. JUnit XML is the test report format
that most CI tools read. A passed test case carries a `message` property with what the test saw. A
failed one carries the failure and its traceback, as JUnit XML always does.

The bench report goes into the same file, as properties of the test suite, so that one file holds
everything about a run:

| Property | Value |
| --- | --- |
| `server.version` | The version of the server under test |
| `setup.seconds` | How long the start of the run took: the wait for the server, the reset, the power cycle, the init and the wait for the devices on the wire. JUnit counts this time in the time of the first test |
| `report.seconds` | How long the bench report took, with the transition to PREOP that its reads need. JUnit counts this time in the time of the first test too |
| `device.<position>.folder` | The folder of the device in `devices/` |
| `device.<position>.productName` | The product name that `GET /api/devices` reports |
| `device.<position>.productCode` | The product code, in hexadecimal |
| `device.<position>.revisionNumber` | The revision number, in hexadecimal |
| `device.<position>.firmwareVersion` | The value of object `0x100A` |
| `device.<position>.buildDescriptor` | The build descriptor from the hardware description |
| `device.<position>.serialNumber` | The serial number from the hardware description |
| `device.<position>.differences` | Where the device differs from its folder, separated by `;` |
| `device.<position>.failedReads` | The reads that failed, separated by `;` |
| `report.error` | Only when the bench report failed: why |

A field that the device did not give is empty. Every device has every property, so a reader can
rely on the keys.

## Run the tests

A run starts with one power cycle. Each test then brings the bench to the start it needs. A test
does not leave the bus as it found it. When the last test ends, passed or not, the run switches the drives
off.

1. Start the server, and connect the bench to it.
2. Install the dependencies.

   ```bash
   cd hil/bench
   source setup.sh
   ```

   `setup.sh` creates a virtual environment in `.venv`, activates it, and installs
   `requirements.txt` into it. Source the script, so that the activation stays in your shell. On
   Debian and Ubuntu, Python's `venv` module is in the package `python3-venv`, which
   `tools/install-deps.sh` installs.

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
`ci-belgrade-linux`. Start it by hand from the Actions tab. It has three inputs:

| Input | Empty | Set |
| --- | --- | --- |
| `version` | Build the Docker image of the server from the selected branch. | Pull `synapticon/motion-master:<version>` from Docker Hub and test that image. A version that is not on Docker Hub fails the run before any test runs. |
| `tests` | Run every test. | Run the tests that a pytest `-k` expression selects, for example `scan and not power`. |
| `process-data-dumps` | Off, the default: save no dump. | On: save a process-data dump of each test that asks for one, and upload it. |

To start the workflow from a terminal, use the GitHub CLI. `--ref` selects the branch that the
tests come from, and the image is built from that branch too when `version` is empty.

```bash
# Every test, with a server built from main
gh workflow run bench-tests.yml --ref main

# Some tests, against a released image, with the process-data dumps
gh workflow run bench-tests.yml --ref main \
  -f version=6.0.0-alpha.122 \
  -f tests="scan and not power" \
  -f process-data-dumps=true
```

Then follow the run, and download its files when it ends:

```bash
gh run list --workflow bench-tests.yml --limit 1   # the run and its ID
gh run watch <run-id>                              # follow it until it ends
gh run download <run-id>                           # report.xml, server.log and the dumps
```

The tests always come from the selected branch, also when the server is a pulled image. The job
summary names the server that ran. The workflow starts the server with the capabilities that SOEM
needs. When the tests end, it uploads `report.xml`, the report file, `server.log`, the log of the
server, and the process-data dumps when the input `process-data-dumps` is on.

The run page shows two tables below the job graph, in the job summary: the bench report, and one
row for each test with its result and its message.

Only one run uses the bench at a time. A second run waits.
