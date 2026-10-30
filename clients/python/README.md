# Motion Master reference Python client

This is a reference Python client for the Motion Master HTTP API and the WebSocket.
At startup it reads the OpenAPI spec from the server and calls each operation by its
`operationId`. See [Why it reads the spec](#why-it-reads-the-spec).

It is easy to start, and nothing must be built. Clone this repository, set up the client, and
run a script. Then use the scripts as a starting point for your own programs. We recommend an AI
coding assistant, such as Claude Code. Open it in `clients/python` and tell it what your program
must do. It reads the client, the scripts and `swagger.yml`, and writes the program.

The recording below shows a first run. The steps are in [Setup](#setup) and [Run](#run).

![A first run of the Python client](demo.gif)

## Setup

Python 3.11 or newer is necessary, because the client reads the config with the standard library
module `tomllib`.

```bash
git clone https://github.com/synapticon/motion-master.git
cd motion-master/clients/python
source setup.sh
cp config.example.toml config.toml
```

`setup.sh` creates a virtual environment in `.venv`, activates it, upgrades pip in it, and
installs `requirements.txt` into it. A virtual environment is a directory that holds the
dependencies of this client apart from your system Python. Source the script, so that the
activation stays in your shell. Git ignores `.venv`. To remove the environment, delete `.venv`.

To do the same by hand, for example on Windows:

```bash
python -m venv .venv
source .venv/bin/activate          # Windows: .venv\Scripts\activate
python -m pip install --upgrade pip
pip install -r requirements.txt
```

Then edit `config.toml`. Set `adapter` in the `[fieldbus]` table to the network interface that
the drives are connected to. Give an interface name, such as `eth0`, or a MAC address.
`python examples/server-info.py` lists the network adapters of the host when the server runs.

## Run

1. Download a build from the [releases](https://github.com/synapticon/motion-master/releases).
2. Start the binary. It listens on `127.0.0.1` by default.
3. Run a script, for example `python examples/scan.py`.

## Scripts

Any script can run first, because it brings the server to the state it needs. A script that
needs a bus opens the fieldbus driver from `config.toml` when no driver is open. It scans when no
devices are known. A script that addresses one device takes `--slave`, the position of the device
on the bus. The default is 1. Each script prints its options with `--help`.

```bash
python examples/scan.py
python examples/transition-state.py --state op
python examples/read-sdo.py --index 0x6064 --subindex 0
python examples/read-file.py --filename .hardware_description
python examples/read-all-parameters.py --slave 2
python examples/monitor.py --count 10
python examples/reset.py
```

**The running server is shared.** A Console session that is connected to the same server sees
every change that a script makes. A script that opens the driver, scans, resets or moves a
device to another AL state changes what the Console shows.

The AL state (Application Layer state, the EtherCAT state machine of a device) column gives
the lowest state the script needs. A script moves the device up to that state when the device is
lower. A device that is already higher stays where it is. The states, from low to high, are INIT,
PRE-OP, SAFE-OP and OP. BOOT is separate: a device enters and leaves BOOT only through INIT.

Three values in that column are not states:

- "Scanned" means that the script needs the devices known. It opens the driver and scans when
  needed, but it moves no device.
- "Driver open" means that the script opens the driver when none is open, and scans nothing.
- "No bus" means that the script opens no driver and leaves the server as it is.

### Server and bus

| Script | What it does | AL state |
| --- | --- | --- |
| `server-info.py` | Prints the server version, the host, the network adapters and the health of the game loop. | No bus |
| `set-game-loop-period.py` | Changes the cycle period of the real-time game loop until the server stops. | No bus |
| `notifications.py` | Prints the notifications that the server sends over the WebSocket. | No bus |
| `scan.py` | Scans the bus and lists the devices it finds. A scan leaves every device in INIT. | Scanned |
| `transition-state.py` | Moves the devices to the AL state you give with `--state`, through every state between. | The state you give |
| `restart-devices.py` | Restarts devices through INIT up to an AL state. An enabled drive is disabled. | Scanned |
| `reset.py` | Closes the fieldbus driver and clears the device list. | No bus |
| `diagnostics.py` | Prints the ESC (EtherCAT Slave Controller) error counters and the distributed clock sync of the devices. | Scanned |
| `capture.py` | Records the EtherCAT frames on the bus adapter for some seconds, and prints how late the frames came back. | Driver open |

### Parameters

| Script | What it does | AL state |
| --- | --- | --- |
| `read-parameter.py` | Reads one parameter with its name, data type and decoded value. `--cache` reads no bus. | PRE-OP |
| `write-parameter.py` | Writes one parameter. It converts the value to the data type, and prints the value before and after. | PRE-OP |
| `read-all-parameters.py` | Reads the whole object dictionary of the device with the value of every entry. | PRE-OP |
| `read-sdo.py` | Reads one object with an SDO upload (Service Data Object, a mailbox read of one object dictionary entry), and prints its raw bytes. | PRE-OP |
| `write-sdo.py` | Writes raw bytes to one object with an SDO download. | PRE-OP |
| `store-parameters.py` | Stores the current parameter values in the flash of the device. | PRE-OP |
| `restore-default-parameters.py` | Restores the default values of a parameter group. **Values not stored are gone.** | PRE-OP |

### Files and firmware

| Script | What it does | AL state |
| --- | --- | --- |
| `list-files.py` | Lists the files stored on the device with their sizes. | PRE-OP |
| `read-file.py` | Reads a file from the device over FoE (File access over EtherCAT), and saves it in `files/`. | PRE-OP |
| `write-file.py` | Writes a local file to the device over FoE. A file of the same name is replaced. | PRE-OP |
| `delete-file.py` | Deletes a file from the flash of a SOMANET device. | PRE-OP |
| `read-hardware-description.py` | Prints what a SOMANET device says it is: the device, the assembly and their components. | PRE-OP |
| `check-firmware-compatibility.py` | Says whether a firmware package belongs on the device, from the package filename alone. It installs nothing. | PRE-OP |
| `firmware-installation.py` | Uploads a firmware package to the server and installs it on the device. The device is not controlled while it is off OP. | Scanned |

### ESC registers and SII

| Script | What it does | AL state |
| --- | --- | --- |
| `read-register.py` | Reads bytes from an ESC register. | Scanned |
| `write-register.py` | Writes bytes to an ESC register. | Scanned |
| `read-sii.py` | Prints the SII (Slave Information Interface, the configuration in the EEPROM of the device), or saves the raw image with `--output`. | Scanned |
| `write-sii.py` | Writes a raw SII image to the EEPROM. **A wrong image can leave the device unidentifiable.** The device adopts it after a power cycle. | Scanned |

### Process data

| Script | What it does | AL state |
| --- | --- | --- |
| `read-pdo-mapping.py` | Prints the PDO (Process Data Object) mapping of the device, with the bit offset and the name of each entry. | PRE-OP |
| `write-pdo-mapping.py` | Writes the PDO mapping from a JSON file. It moves the device to PRE-OP for the write, and back to SAFE-OP or OP. | PRE-OP for the write |
| `monitor.py` | Creates a monitoring and prints its batches of process data from the WebSocket. | OP |
| `dump-process-data.py` | Saves the recorder of the server, every recorded cycle of process data, as a `.mmpd` file in `files/`. | SAFE-OP |

### Drive

| Script | What it does | AL state |
| --- | --- | --- |
| `cia402-state.py` | Prints the CiA 402 state of the drive with its actual and target values. | PRE-OP |
| `set-target.py` | Sets the position, velocity or torque target. **An enabled drive moves to it at once.** | PRE-OP |
| `release-brake.py` | Releases the brake of a SOMANET drive. It releases only while the drive is in Operation Enabled, and stays released. | PRE-OP |
| `engage-brake.py` | Engages the brake of a SOMANET drive. | PRE-OP |
| `device-locate.py` | Blinks the LEDs of the device. Only SPoE offers it. | PRE-OP |

### Procedures

| Script | What it does | AL state |
| --- | --- | --- |
| `run-procedure.py` | Lists the procedures of a device. With a name, it runs that procedure with parameters given as `name=value`, and prints each step with its result. `--describe` prints the description, caveats and parameters instead. | PRE-OP, or `--state` |
| `offset-detection.py` | Runs the offset detection procedure on a SOMANET drive, and prints each step with its result. **It turns the motor and releases the brake.** Ctrl-C cancels the run. | OP |
| `system-identification.py` | Configures the system identification chirp, and arms it with `--start`. **Once armed, it turns the motor.** | PRE-OP |
| `plant-identification.py` | Identifies the plant model with a chirp, and writes `plant_model.csv`. **It turns the motor in both directions.** | OP |

### ESI and ENI

| Script | What it does | AL state |
| --- | --- | --- |
| `parse-esi.py` | Parses an ESI (EtherCAT Slave Information) file and lists the devices it describes. | No bus |
| `export-eni.py` | Saves the bus as an ENI (EtherCAT Network Information) file in `files/`, for another master to replay. | SAFE-OP |

### Production

A production script runs many steps in order on one drive, asks before the steps that need a
decision, and writes a log and a record of the drive. Each script has a README with its steps.

| Script | What it does | AL state |
| --- | --- | --- |
| `production/integro/commission.py` | Commissions an Integro drive. | PRE-OP |

## Calls by operationId

Every request goes through one method, `MotionMasterClient.call(operationId, ...)`. It takes
the path parameters as `path`, a JSON body as `body`, and the query string as `query`:

```python
client.call("init", body={"driver": "soem", "adapter": "eth0"})
client.call("sdoUpload", path={"slavePosition": 1, "index": 0x6041, "subindex": 0})
```

The steps that most scripts begin with are methods on the client:

```python
client = MotionMasterClient.from_config()     # reads config.toml
client.init()                                 # opens the driver, or accepts the one that is open
client.ensure_state(PRE_OP, positions=[1])    # scans when needed, then moves device 1 up
```

`ensure_scanned()` scans when needed and moves no device. `init()`, `ensure_scanned()` and
`ensure_state()` log through the `motion_master_client` logger. Turn on logging at INFO to see
each step.

Other methods:

- `run_procedure()` runs a procedure to its end.
- `monitor(topic)` streams the batches of a monitoring from the WebSocket.
- `notifications()` streams the notifications from the WebSocket.
- `run(main)` calls `main` and prints an error that a user can correct as one line instead of a
  traceback. An example is a server that is not running. Set `MM_TRACEBACK=1` to get the
  traceback.

## TLS

Motion Master accepts TLS connections only. A release binary includes a Let's Encrypt
certificate for `local.motion-master.synapticon.com`, which resolves to `127.0.0.1`. A public CA
(certificate authority, an organisation that operating systems trust to sign certificates) signs
it. So with the default `host` you can set `verify = true`. The client then verifies the
certificate against the CA store of the system, with no other setup.

The template sets `verify = false`, because that works with every certificate. This includes the
self-signed certificate that the binary uses when it cannot download a real one, for example on a
host with no internet access. Set `verify = true`, or set `verify` to the path of a `cert.pem`,
when you know which certificate the server presents.

## Two ports

Motion Master serves two endpoints, each on its own event loop. The HTTP API is on `http_port`,
61447. The WebSocket is on `ws_port`, 62281. That is why `host` in the config has no port.
`client.call(...)` uses the HTTP API. `client.monitor(topic)` and `client.notifications()` use
the WebSocket. `monitor.py` and `notifications.py` show the WebSocket.

## Why it reads the spec

The scripts name an `operationId`, such as `init` or `sdoUpload`. They never name a URL or an
HTTP method. At startup the client reads `GET /api/swagger.yml` and builds one table from
`operationId` to the method and the URL template. `call()` looks up the ID, fills the
placeholders of the template from `path`, and sends the request. So the URLs and the methods
are in one place only, the spec of the server.

That gives four results:

- **There is one source of truth.** The same binary implements the API and serves the spec. URLs
  in the scripts would be a second copy of the contract, written by hand, and it would be wrong
  after the API changes. An `operationId` is a stable name, so a script keeps working when a URL
  on the server changes.
- **The client always matches its server.** The spec comes from the running server. So the client
  is correct against the contract of that server, older or newer.
- **The client is standalone.** There is no bundled spec, no code generation and no checkout of
  this repository. Copy `motion_master_client.py`, install the requirements, and point it at a
  server.
- **A script shows the API.** Each call names the operation that it invokes. There is no URL
  string to read past.

The cost is one request at startup, and the server must be reachable when the client is built.

## Layout

| File | Purpose |
| --- | --- |
| `motion_master_client.py` | `MotionMasterClient`, which calls the HTTP API and the WebSocket. See [Calls by operationId](#calls-by-operationid). |
| `setup.sh` | Creates and activates the virtual environment. See [Setup](#setup). |
| `requirements.txt` | The Python packages that the client needs. |
| `config.example.toml` | The config template. Copy it to `config.toml`. |
| `demo.tape` | The [VHS](https://github.com/charmbracelet/vhs) script that records `demo.gif`. Run `vhs demo.tape`. |
| `examples/` | The example scripts. See [Scripts](#scripts). |
| `files/` | Where a script saves a file when `--output` names no other path. Git ignores this directory. |
| `swagger.yml` | A copy of the spec that the client read from the server at its last startup. Look up an `operationId` here when you write a new script. The client writes it again at each startup, and Git ignores it. |
