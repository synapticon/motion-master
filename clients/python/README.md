# Motion Master reference Python client

This is a reference Python client for the Motion Master HTTP API and the monitoring WebSocket.
At startup it reads the server's own OpenAPI spec from `GET /api/swagger.yml`, and it calls each
operation by its `operationId`. [Why it reads the spec](#why-it-reads-the-spec) explains the
reason.

Each script in `examples/` does one action and is named after it. Each script runs on its own.
It brings the server to the state it needs, so no script has to run before another one. A
second run gives the same result as the first run.

## Layout

| File | Purpose |
| --- | --- |
| `motion_master_client.py` | `MotionMasterClient`. It calls the HTTP API by `operationId` and streams the monitoring WebSocket. It also has the steps that most scripts begin with: `from_config()`, `init()`, `ensure_scanned()` and `ensure_state()`. `run_procedure()` runs a procedure to its end, and `notifications()` streams the notifications. |
| `config.example.toml` | The config template. Copy it to `config.toml`. |
| `examples/` | One script per action. See [Scripts](#scripts). |
| `files/` | Where a script saves a file when `--output` names no other path. Git ignores this directory. |

## Scripts

The AL state (Application Layer state, the EtherCAT state machine of a device) column gives
the lowest state the script needs. A script moves the device up to that state when the device is
lower. A device that is already higher stays where it is. The states, from low to high, are INIT,
PRE-OP, SAFE-OP and OP. BOOT is separate: a device enters and leaves BOOT only through INIT.

Three values in that column are not states. "None" means that the script needs the devices known,
so it opens the driver and scans when needed, but it moves no device. "No bus" means that the
script opens no driver and leaves the server as it is. "Driver open" means that the script opens
the driver when none is open, and scans nothing.

| Script | What it does | AL state |
| --- | --- | --- |
| `scan.py` | Scans the bus and lists the devices it finds. A scan leaves every device in INIT. | None |
| `transition-state.py` | Moves the devices to the AL state you give with `--state`, through every state between. | The state you give |
| `reset.py` | Closes the fieldbus driver and clears the device list. | No bus |
| `read-all-parameters.py` | Reads the whole object dictionary of the device with the value of every entry. | PRE-OP |
| `read-sdo.py` | Reads one object with an SDO upload (Service Data Object, a mailbox read of one object dictionary entry), and prints its raw bytes. | PRE-OP |
| `read-file.py` | Reads a file from the device over FoE (File access over EtherCAT), and saves it in `files/`. | PRE-OP |
| `monitor.py` | Creates a monitoring and prints its batches of process data from the WebSocket. | OP |
| `run-procedure.py` | Lists the procedures of a device. With a name, it runs that procedure with parameters given as `name=value`, and prints each step with its result. `--describe` prints the description, caveats and parameters instead. | PRE-OP, or `--state` |
| `offset-detection.py` | Runs the offset detection procedure on a SOMANET drive, and prints each step with its result. **It turns the motor and releases the brake.** Ctrl-C cancels the run. | OP |

Every script that needs a bus opens the fieldbus driver from `config.toml` when no driver is
open, and scans when no devices are known. A script that addresses one device takes `--slave`,
the position of the device on the bus. The default is 1.

```bash
python examples/scan.py
python examples/transition-state.py --state op
python examples/read-sdo.py --index 0x6064 --subindex 0
python examples/read-file.py --filename .hardware_description
python examples/read-all-parameters.py --slave 2
python examples/monitor.py --count 10
python examples/reset.py
```

Each script prints its options with `--help`.

**The running server is shared.** A Console session that is connected to the same server sees
every change that a script makes. A script that opens the driver, scans, resets or moves a
device to another AL state changes what the Console shows.

## Setup

Python 3.11 or newer is necessary, because the client reads the config with the standard library
module `tomllib`. The client is standalone. It reads the API spec from the running server, so it
needs no checkout of this repository.

```bash
cd clients/python
source setup.sh
cp config.example.toml config.toml
```

`setup.sh` creates a virtual environment in `.venv`, activates it, and installs
`requirements.txt` into it. A virtual environment is a directory that holds the dependencies of
this client apart from your system Python. Git ignores `.venv`. To remove everything, delete the
directory. Source the script, so that the activation stays in your shell.

To do the same by hand, for example on Windows:

```bash
python -m venv .venv
source .venv/bin/activate          # Windows: .venv\Scripts\activate
pip install -r requirements.txt
```

Then edit `config.toml`. Set `adapter` in the `[fieldbus]` table to the network interface that
the drives are connected to. Give an interface name, such as `eth0`, or a MAC address.
`GET /api/adapters` on a running server lists the interfaces of the host.

## Run

1. Download a build from the [releases](https://github.com/synapticon/motion-master/releases).
2. Start the binary. It listens on `127.0.0.1` by default.
3. Run a script.

The scripts work against a server with no driver open, and against a server with a driver open.

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

`init()` and `ensure_state()` log through the `motion_master_client` logger. Turn on logging at
INFO to see each step.

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

The client parses the spec with `yaml.safe_load`. It needs only the `operationId`, the method and
the path of each operation, so a plain YAML load is enough. A strict OpenAPI validator would also
refuse legal media types, such as the `text/yaml` that this endpoint declares.

The cost is one request at startup, and the server must be reachable when the client is built.
For an example client that can do nothing without the server, that costs nothing. The TypeScript
client makes the opposite choice. It builds the contract in when it is compiled, so it needs no
request at startup and no server to construct. In exchange, it must be generated again whenever
the spec changes.

## TLS

Motion Master accepts TLS connections only. A release binary includes a Let's Encrypt
certificate for `local.motion-master.synapticon.com`, which resolves to `127.0.0.1`. A public
CA signs it, so with the default `host` you can set `verify = true`. The client then verifies the
certificate against the CA store of the system, with no other setup.

The template sets `verify = false`, because that works with every certificate. This includes the
self-signed certificate that the binary uses when it cannot download a real one, for example on a
host with no internet access. Set `verify = true`, or set `verify` to the path of a `cert.pem`,
when you know which certificate the server presents.

## Two ports

Motion Master serves two endpoints, each on its own event loop. The HTTP API is on `http_port`,
61447. The monitoring WebSocket is on `ws_port`, 62281. That is why `host` in the config has no
port. `MotionMasterClient` holds both endpoints: `client.call(...)` uses the HTTP API, and
`client.monitor(topic)` streams batches from the WebSocket. `monitor.py` shows the second one.
