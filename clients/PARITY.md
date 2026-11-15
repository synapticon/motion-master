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

Three rules turn a Python signature into a TypeScript one:

- A required argument is positional in both clients.
- An optional argument is a keyword argument in Python and a field of a trailing options object in
  TypeScript, with the same name: `ensure_state(state, positions=[1], exact=True)` is
  `ensureState(state, { positions: [1], exact: true })`. A TypeScript helper that waits also takes
  `signal`, an `AbortSignal`, in that object.
- A time carries its unit in its name, in milliseconds unless the API uses another unit:
  `timeout_ms` is `timeoutMs`.

Both clients log each step a helper takes, such as "found 1 device(s)". The log is off by default.
Python logs through the `motion_master_client` logger at INFO. TypeScript logs through the `debug`
package under the name `motion-master-client`: set `DEBUG=motion-master-client` in Node.js, or
`localStorage.debug = 'motion-master-client'` in a browser.

This file is the parity table. `tools/check-client-parity.mjs` compares it with the public names
of both clients and fails when they differ:

```bash
node tools/check-client-parity.mjs
```

The check reads these names:

| Section | Python | TypeScript |
| --- | --- | --- |
| Module | The public classes, functions and `UPPER_CASE` constants of `motion_master_client.py`, and the names it imports from `motion_master_objects.py` | The exports of `src/index.ts`, except the generated `Api`, `HttpClient` and `ContentType` |
| A class | The public methods, class methods and properties of the class | The public methods, static methods and getters of the class |

A row with a name in one column only belongs to that language. Its reason says why. A row marked
`pending` is not built yet.

## Module

| Python | TypeScript | Status | Reason |
| --- | --- | --- | --- |
| `MotionMasterClient` | `MotionMasterClient` | | |
| `MonitoringRecording` | `MonitoringRecording` | | |
| `AlState` | `AlState` | | |
| `Cia402State` | `Cia402State` | | |
| `OperationMode` | `OperationMode` | | |
| `Cia402` | `Cia402` | | |
| `Profile` | `Profile` | | |
| `Somanet` | `Somanet` | | |
| `ObjectAddress` | `ObjectAddress` | | |
| `StateError` | `StateError` | | |
| `ProcedureError` | `ProcedureError` | | |
| `CaptureError` | `CaptureError` | | |
| `ProcessDataDumpError` | — | pending | |
| `step_changes` | `stepChanges` | | |
| `format_step` | `formatStep` | | |
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
| `NOTIFICATION_TOPIC` | `NOTIFICATION_TOPIC` | | |
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
| `from_config` | `fromConfig` | | |
| `call` | — | | TypeScript calls an operation through the generated `client.api`, which the build types from `swagger.yml`. Python has no generated client, so `call` finds the operation in the spec of the running server. |
| — | `close` | | Closes the WebSocket of `client.ws`. Python closes the WebSocket of each stream when the stream ends. |
| `init` | `init` | | |
| `reset` | `reset` | | |
| `scan` | `scan` | | |
| `devices` | `devices` | | |
| `parameters` | `parameters` | | |
| `hardware_description` | `hardwareDescription` | | |
| `read_value` | `readValue` | | |
| `wait_for_value` | `waitForValue` | | |
| `ensure_scanned` | `ensureScanned` | | |
| `ensure_state` | `ensureState` | | |
| `restart` | `restart` | | |
| `set_operation_mode` | `setOperationMode` | | |
| `set_target` | `setTarget` | | |
| `enable` | `enable` | | |
| `disable` | `disable` | | |
| `quick_stop` | `quickStop` | | |
| `run_procedure` | `runProcedure` | | |
| `monitoring` | `monitoring` | | |
| `stream_monitoring` | `streamMonitoring` | | |
| `notifications` | `notifications` | | |
| `start_capture` | `startCapture` | | |
| `stop_capture` | `stopCapture` | | |
| `process_data_mark` | `readProcessDataMark` | pending | |
| `process_data_dump` | `fetchProcessDataDump` | pending | |

## MonitoringRecording

| Python | TypeScript | Status | Reason |
| --- | --- | --- | --- |
| `sources` | `sources` | | |
| `names` | `names` | | |
| `column` | `column` | | |
| `values` | `values` | | |
| `first` | `first` | | |
| `write_csv` | `writeCsv` | | |
| `print_table` | `printTable` | | |
