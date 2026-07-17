# Motion Master Python client examples

A small, dependency-light Python client for the Motion Master HTTP API and
monitoring WebSocket, driven directly by the server's own OpenAPI spec, which it
fetches at startup from `GET /api/swagger.yml`: operations are resolved by
`operationId`, never hardcoded as URLs, so the client stays correct as the API
evolves and always matches the exact contract of the server it talks to. Each script demonstrates
one part of the API and runs on its own; against a freshly started,
**uninitialised** Motion Master the numbered scripts also read in order as a
walkthrough of the full fieldbus lifecycle — from bringing up a driver to
streaming live process data and tearing back down.

## Layout

| File | Purpose |
| --- | --- |
| `mm_client.py` | `MMClient` — fetches the server's spec from `GET /api/swagger.yml` (parsed with PyYAML), resolves each operation's method + URL by `operationId`, and calls it with `requests`. Holds both the HTTP and WebSocket endpoints. |
| `common.py` | Loads `config.toml` and builds a wired `MMClient`. |
| `config.example.toml` | Config template — copy to `config.toml`. |
| `examples/01_init.py` | `POST /api/init` — bring up the driver from config. |
| `examples/02_scan.py` | `POST /api/scan` — discover slaves. |
| `examples/03_transition_preop.py` | `POST /api/devices/state` — move all devices to PRE-OP. |
| `examples/04_read_sdo.py` | `GET .../sdo/{index}/{subindex}` — raw CoE SDO upload. |
| `examples/05_read_file.py` | `GET .../files/{filename}` — read a file over FoE. |
| `examples/06_read_parameters.py` | `POST .../parameters/init?readValues=true` — dump the full object dictionary with values. |
| `examples/07_transition_op.py` | `POST /api/devices/state` — climb PRE-OP → SAFE-OP → OP. |
| `examples/08_monitor.py` | `POST /api/monitorings` + WebSocket — stream live process data. |
| `examples/09_reset.py` | `POST /api/reset` — tear the driver down, back to uninitialised. |

Every request goes through one method — `MMClient.call(operationId, ...)` — with
`path` parameters, a JSON `body`, and/or a `query` as the operation needs:

```python
client.call("init", body={"driver": "soem", "adapter": "eth0"})
client.call("sdoUpload", path={"slavePosition": 1, "index": 0x6041, "subindex": 0})
```

## Setup

Requires Python 3.11+ (the config is parsed with the stdlib `tomllib`). The
client is standalone — it reads the API spec from the running server at startup,
so no checkout of this repository is needed.

```bash
cd clients/python
python -m venv .venv && source .venv/bin/activate   # Windows: .venv\Scripts\activate
pip install -r requirements.txt
cp config.example.toml config.toml                  # then edit driver + adapter
```

The `venv` line creates a *virtual environment* — a throwaway `.venv` folder that
holds this example's dependencies in isolation, so they never touch your system
Python. `.venv` is the conventional name (editors auto-detect it) and is
git-ignored; delete the folder to remove everything.

The `adapter` is the NIC the drives are wired to — an interface name (e.g.
`eth0`) or its MAC address. `GET /api/adapters` on a running server lists the
interfaces available on the host.

## Run

Start Motion Master and leave it uninitialised — download a build from the
[releases](https://github.com/synapticon/motion-master/releases) and run the
binary (it listens on `127.0.0.1` by default). Then run the examples in order:

```bash
python examples/01_init.py
python examples/02_scan.py
python examples/03_transition_preop.py
python examples/04_read_sdo.py                      # Statusword 0x6041:00 by default
python examples/04_read_sdo.py --index 0x6064 --subindex 0
python examples/05_read_file.py --filename log.csv
python examples/06_read_parameters.py               # dump the object dictionary + values
python examples/07_transition_op.py                 # climb to OP (exchanging state)
python examples/08_monitor.py --count 10            # stream 10 batches, then stop
python examples/09_reset.py                          # tear down, back to uninitialised
```

The per-device examples (04–06, 08) act on slave 1 by default; pass `--slave N`
to target another. Monitoring is live-only — values read `null` until the device
is exchanging, so `08_monitor.py` needs the bus in OP (step 07); steps 04–06 only
need PRE-OP.

## TLS

Motion Master is TLS-only. A release binary bundles a publicly-trusted Let's
Encrypt certificate for `local.motion-master.synapticon.com` (which resolves to
`127.0.0.1`), so with the default `host` you can set `verify = true` and it
verifies against the system CA store with no extra setup.

The default `config.toml` ships `verify = false` because it works everywhere —
including the self-signed certificate the binary falls back to when it cannot
fetch a real one (offline / air-gapped). Set `verify = true`, or point `verify`
at a `cert.pem`, once you know the server is presenting the bundled cert.

## Two ports

The config keeps `host` portless because Motion Master exposes two endpoints on
separate loops: the HTTP API (`http_port`, 61447) used by most examples, and the
monitoring WebSocket (`ws_port`, 62281) for live process data. `MMClient` holds
both — `client.call(...)` hits the HTTP API, and `client.monitor(topic)` streams
batches from the WebSocket (see `08_monitor.py`).
