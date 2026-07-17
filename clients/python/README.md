# Motion Master reference Python client

A reference Python client for the Motion Master HTTP API and
monitoring WebSocket, driven directly by the server's own OpenAPI spec, which it
fetches at startup from `GET /api/swagger.yml` (see [Why it reads the
spec](#why-it-reads-the-spec)). Each script demonstrates
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

## Why it reads the spec

Notice the scripts name an **`operationId`** — `init`, `sdoUpload` — never a URL or
an HTTP verb. At startup the client fetches `GET /api/swagger.yml`, walks the
spec's `paths`, and builds one lookup table, `operationId → (method, URL
template)`. `call()` then resolves the id, fills the template's `{...}`
placeholders from `path`, and fires the request. The URLs and verbs live in
exactly one place — the server's own spec — and the client reads them from the
source of truth instead of duplicating them. Four things fall out of that:

- **One source of truth, no drift.** The server implements the API *and* serves
  the spec describing it, both from the same binary. Hardcoded URLs would be a
  second, hand-maintained copy of the contract that silently rots when the API
  moves; resolving by `operationId` (a stable logical name) means a URL
  restructuring on the server just flows through — the script keeps working with
  no edit.
- **It always matches the server it's talking to.** The spec comes from the
  *running* server, so the client is correct against *that* server's contract by
  construction — point it at an older or newer Motion Master and it adapts; it
  can't be a stale client against a fresh server.
- **It's standalone.** Because the contract is fetched live, there's no bundled
  spec, no code generation, and no checkout of this repository — copy the four
  `.py` files anywhere, `pip install`, and point at a server.
- **It reads as a teaching client.** Each call says "invoke *this named
  operation*" — a clean tour of the API surface — instead of a pile of URL
  string-building that hides what's being shown.

The spec is parsed with a plain `yaml.safe_load`: the client only needs
`operationId → method + path`, so a generic YAML load is enough — and it avoids a
strict OpenAPI validator rejecting perfectly legal media types such as the
`text/yaml` this very endpoint declares.

The cost is one round-trip at startup and needing the server reachable when the
client is built — free for an examples client that can't do anything until the
server is up. The committed TypeScript client makes the opposite trade: it bakes
the contract in at build time (zero startup cost, offline construction) at the
price of regeneration whenever the spec changes. Different tool, different job.

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
