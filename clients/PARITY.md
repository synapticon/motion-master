# Client parity

Motion Master has two client libraries:

- The Python client, `clients/python/motion_master_client.py`.
- The TypeScript client, `web/packages/motion-master-client`.

The two clients offer the same helpers. A helper has the same name in both clients, word by word.
Python writes the name in `snake_case`, and TypeScript writes it in `camelCase`, so
`wait_for_value` is `waitForValue`. A class or a constant has the same name in both clients, for
example `AlState` and `MMPD_FLAG_INCOMPLETE_START`. A helper takes the same arguments in the same
order, and it returns the same shape.

Each language keeps its own idiom. Python streams with generators and `with` blocks. TypeScript
streams with listeners, `AsyncIterable` and promises.

This file is the parity table. `tools/check-client-parity.mjs` compares it with the public names
of both clients and fails when they differ:

```bash
node tools/check-client-parity.mjs
```

The check reads these names:

| Section | Python | TypeScript |
| --- | --- | --- |
| Module | The public classes, functions and `UPPER_CASE` constants of `motion_master_client.py`, and the names it imports from `motion_master_objects.py` | The exports of `src/index.ts`, except the generated `Api`, `HttpClient` and `ContentType` |
| A class | The public methods and properties of the class | The public methods and getters of the class |

A row with a name in one column only belongs to that language. Its reason says why. A row marked
`pending` is not built yet.

## Module

| Python | TypeScript | Status | Reason |
| --- | --- | --- | --- |
| `MotionMasterClient` | `MotionMasterClient` | | |
| `Monitoring` | — | pending | |
| `AlState` | `AlState` | | |
| `Cia402State` | `Cia402State` | | |
| `OperationMode` | `OperationMode` | | |
| `Cia402` | `Cia402` | | |
| `Profile` | `Profile` | | |
| `Somanet` | `Somanet` | | |
| `ObjectAddress` | `ObjectAddress` | | |
| `StateError` | — | pending | |
| `ProcedureError` | — | pending | |
| `CaptureError` | — | pending | |
| `ProcessDataDumpError` | — | pending | |
| `step_changes` | — | pending | |
| `format_step` | — | pending | |
| `error_line` | `apiErrorMessage` | pending | |
| `run` | — | pending | |
| `MMPD_FLAG_INCOMPLETE_START` | `MMPD_FLAG_INCOMPLETE_START` | | |
| `mmpd_flags` | — | pending | |
| — | `MmpdFile` | pending | |
| — | `parseMmpd` | pending | |
| — | `fetchProcessDataDump` | pending | |
| — | `decodeMmpdValue` | pending | |
| — | `extractBits` | pending | |
| — | `isPlottableDataType` | pending | |
| — | `fetchEni` | pending | |
| — | `parseEni` | pending | |
| — | `NOTIFICATION_TOPIC` | pending | |
| — | `WebSocketConnection` | | The WebSocket connection behind `client.ws`. It reconnects and routes each message to its listeners. Python opens one WebSocket for each stream and closes it when the stream ends. |
| — | `alStateName` | | Python reads the name from the enum: `AlState(2).name`. |
| — | `operationModeName` | | Python reads the name from the enum: `OperationMode(9).name`. |
| — | `API_BASE_URL` | | The default address of the HTTP API for a web app. Python reads the host and the ports from `config.toml`. |
| — | `WS_URL` | | The default address of the WebSocket for a web app. Python reads the host and the ports from `config.toml`. |
| — | `LAN_DOMAIN` | | Browser only. A browser reaches a server on another computer under a name that the bundled certificate covers. |
| — | `lanAddress` | | Browser only, as `LAN_DOMAIN`. |
| — | `lanHostname` | | Browser only, as `LAN_DOMAIN`. |
| — | `isIpv4` | | Browser only, as `LAN_DOMAIN`. |
| — | `WIRE_TIME_HEADER` | | The Console shows the time a request spent on the wire. A Python script reads the header from `response.headers`. |
| — | `wireTimeMs` | | As `WIRE_TIME_HEADER`. |
| — | `formatHex` | | The hex input fields of the Console. Python formats and parses hex with `f"{value:#x}"` and `int(text, 16)`. |
| — | `parseHexBytes` | | As `formatHex`. |
| — | `parseIntegerFlexible` | | As `formatHex`. |
| — | `SDO_TYPES` | | The SDO page of the Console encodes and decodes raw SDO bytes. A Python script reads and writes a value with `read_value` and the server decodes it. |
| — | `SDO_TYPE_HINT` | | As `SDO_TYPES`. |
| — | `decodeSdoBytes` | | As `SDO_TYPES`. |
| — | `encodeSdoValue` | | As `SDO_TYPES`. |
| — | `interpretSdoBytes` | | As `SDO_TYPES`. |
| — | `isIntegerSdoType` | | As `SDO_TYPES`. |
| — | `sdoTypeForDataTypeName` | | As `SDO_TYPES`. |

## MotionMasterClient

| Python | TypeScript | Status | Reason |
| --- | --- | --- | --- |
| `from_config` | — | pending | |
| `call` | — | | TypeScript calls an operation through the generated `client.api`, which the build types from `swagger.yml`. Python has no generated client, so `call` finds the operation in the spec of the running server. |
| — | `close` | | Closes the WebSocket of `client.ws`. Python closes the WebSocket of each stream when the stream ends. |
| `init` | — | pending | |
| `reset` | — | pending | |
| `scan` | — | pending | |
| `devices` | — | pending | |
| `parameters` | — | pending | |
| `hardware_description` | — | pending | |
| `read_value` | `readValue` | | |
| `wait_for_value` | — | pending | |
| `ensure_scanned` | — | pending | |
| `ensure_state` | — | pending | |
| `restart` | — | pending | |
| `set_operation_mode` | — | pending | |
| `set_target` | — | pending | |
| `enable` | — | pending | |
| `disable` | — | pending | |
| `quick_stop` | — | pending | |
| `run_procedure` | — | pending | |
| `monitoring` | — | pending | |
| `monitor` | — | pending | |
| `notifications` | — | pending | |
| `start_capture` | — | pending | |
| `stop_capture` | — | pending | |
| `process_data_mark` | `readProcessDataMark` | pending | |
| `process_data_dump` | `fetchProcessDataDump` | pending | |

## Monitoring

| Python | TypeScript | Status | Reason |
| --- | --- | --- | --- |
| `sources` | — | pending | |
| `names` | — | pending | |
| `column` | — | pending | |
| `values` | — | pending | |
| `first` | — | pending | |
| `write_csv` | — | pending | |
| `print_table` | — | pending | |
