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
and the example programs of both clients, and fails when they differ:

```bash
node tools/check-client-parity.mjs
```

The check reads these names:

| Section | Python | TypeScript |
| --- | --- | --- |
| Module | The public classes, functions and `UPPER_CASE` constants of `motion_master_client.py`, and the names it imports from `motion_master_objects.py` | The exports of `src/index.ts`, except the generated `Api`, `HttpClient` and `ContentType` |
| A class | The public methods, class methods and properties of the class | The public methods, static methods and getters of the class |
| Examples | The programs in `clients/python/examples/`, except the helpers `files.py`, `options.py` and `procedures.py` | The programs in `web/packages/motion-master-client/examples/`, except the helpers `files.ts`, `options.ts` and `procedures.ts` |

A row with a name in one column only belongs to that language. Its reason says why.

## Module

| Python | TypeScript | Reason |
| --- | --- | --- |
| `MotionMasterClient` | `MotionMasterClient` | |
| `MonitoringRecording` | `MonitoringRecording` | |
| `AlState` | `AlState` | |
| `Cia402State` | `Cia402State` | |
| `OperationMode` | `OperationMode` | |
| `Cia402` | `Cia402` | |
| `Profile` | `Profile` | |
| `Somanet` | `Somanet` | |
| `ObjectAddress` | `ObjectAddress` | |
| `StateError` | `StateError` | |
| `ProcedureError` | `ProcedureError` | |
| `CaptureError` | `CaptureError` | |
| `MonitoringError` | `MonitoringError` | |
| `ProcessDataDumpError` | `ProcessDataDumpError` | |
| — | `ConnectionError` | Python raises `requests.ConnectionError`, which carries the URL of the request. `fetch` throws a bare `TypeError`, so the TypeScript client throws its own error. |
| — | `HttpError` | Python raises `requests.HTTPError`, which carries the method, the status and the body. A refused `fetch` throws nothing, so the TypeScript client throws its own error. |
| `step_changes` | `stepChanges` | |
| `object_name` | `objectName` | |
| `object_address_text` | `objectAddressText` | |
| `format_step` | `formatStep` | |
| `error_line` | `errorLine` | |
| — | `apiErrorMessage` | Returns a message for any error, with a fallback, for a page to show. `errorLine` returns `null` for a bug, so that a program shows its stack trace. |
| `run` | `run` | |
| `MMPD_FLAG_INCOMPLETE_START` | `MMPD_FLAG_INCOMPLETE_START` | |
| `mmpd_flags` | — | Returns the flags of a `.mmpd` file. TypeScript reads them from the header that `parseMmpd()` decodes: `parseMmpd(bytes).header.flags`. |
| — | `MmpdFile` | The decoded `.mmpd` file. The Console plots a dump in the browser, and the Python client has no decoder of the format yet. |
| — | `parseMmpd` | Decodes a `.mmpd` file. As `MmpdFile`. |
| — | `decodeMmpdValue` | Decodes one value of a `.mmpd` row. As `MmpdFile`. |
| — | `extractBits` | Takes the bits of one value out of a `.mmpd` row. As `MmpdFile`. |
| — | `isPlottableDataType` | Says whether the Console can plot a data type. As `MmpdFile`. |
| `NOTIFICATION_TOPIC` | `NOTIFICATION_TOPIC` | |
| — | `WebSocketConnection` | The WebSocket connection behind `client.ws`. It reconnects and routes each message to its listeners. Python opens one WebSocket for each stream and closes it when the stream ends. |
| — | `alStateName` | Python reads the name from the enum: `AlState(2).name`. |
| — | `operationModeName` | Python reads the name from the enum: `OperationMode(9).name`. |
| — | `API_BASE_URL` | The default address of the HTTP API for a web app. Python reads the host and the ports from `config.toml`. |
| — | `WS_URL` | The default address of the WebSocket for a web app. Python reads the host and the ports from `config.toml`. |
| — | `LAN_DOMAIN` | A program reaches a server on another computer under a name that the bundled certificate covers. Python takes the host from `config.toml`, so a script writes that name there. |
| — | `lanAddress` | As `LAN_DOMAIN`. |
| — | `lanHostname` | As `LAN_DOMAIN`. |
| — | `isIpv4` | As `LAN_DOMAIN`. |
| `wire_time_ms` | `wireTimeMs` | |
| — | `formatHex` | The hex input fields of the Console. Python formats and parses hex with `f"{value:#x}"` and `int(text, 16)`. |
| — | `formatHexBytes` | Python writes the same with `data.hex(" ").upper()`. |
| — | `parseHexBytes` | As `formatHex`. |
| — | `parseIntegerFlexible` | As `formatHex`. |
| — | `integerFromBytes` | Python has `int.from_bytes(data, "little", signed=...)` built in. |
| — | `SDO_TYPES` | The SDO page of the Console encodes and decodes raw SDO bytes. A Python script reads and writes a value with `read_value` and the server decodes it. |
| — | `SDO_TYPE_HINT` | As `SDO_TYPES`. |
| — | `decodeSdoBytes` | As `SDO_TYPES`. |
| — | `encodeSdoValue` | As `SDO_TYPES`. |
| — | `interpretSdoBytes` | As `SDO_TYPES`. |
| — | `isIntegerSdoType` | As `SDO_TYPES`. |
| — | `sdoTypeForDataTypeName` | As `SDO_TYPES`. |

## MotionMasterClient

| Python | TypeScript | Reason |
| --- | --- | --- |
| `from_config` | `fromConfig` | |
| `call` | — | TypeScript calls an operation through the generated `client.api`, which `pnpm generate` writes from `swagger.yml`. Python has no generated client, so `call` finds the operation in the spec of the running server. |
| — | `close` | Closes the WebSocket of `client.ws`. Python closes the WebSocket of each stream when the stream ends. |
| `init` | `init` | |
| `reset` | `reset` | |
| `scan` | `scan` | |
| `devices` | `devices` | |
| `parameters` | `parameters` | |
| `hardware_description` | `hardwareDescription` | |
| `read_value` | `readValue` | |
| `write_value` | `writeValue` | |
| `read_sdo` | `readSdo` | |
| `write_sdo` | `writeSdo` | |
| `list_files` | `listFiles` | |
| `read_file` | `readFile` | |
| `write_file` | `writeFile` | |
| `remove_file` | `removeFile` | |
| `read_sii` | `readSii` | |
| `write_sii` | `writeSii` | |
| `wait_for_value` | `waitForValue` | |
| `ensure_scanned` | `ensureScanned` | |
| `ensure_state` | `ensureState` | |
| `restart` | `restart` | |
| `set_operation_mode` | `setOperationMode` | |
| `set_target` | `setTarget` | |
| `enable` | `enable` | |
| `disable` | `disable` | |
| `quick_stop` | `quickStop` | |
| `run_procedure` | `runProcedure` | |
| `monitoring` | `monitoring` | |
| `stream_monitoring` | `streamMonitoring` | |
| `notifications` | `notifications` | |
| `start_capture` | `startCapture` | |
| `export_eni` | `exportEni` | |
| `parse_eni` | `parseEni` | |
| `stop_capture` | `stopCapture` | |
| `process_data_mark` | `processDataMark` | |
| `process_data_dump` | `processDataDump` | |

## MonitoringRecording

| Python | TypeScript | Reason |
| --- | --- | --- |
| `sources` | `sources` | |
| `names` | `names` | |
| `column` | `column` | |
| `values` | `values` | |
| `first` | `first` | |
| `write_csv` | `writeCsv` | |
| `print_table` | `printTable` | |

## Examples

Each client has one example program per task, with the same name, the same options and the same
output. A row names the two files. The scripts in `clients/python/examples/production/` are tools
for one board each, not examples of the client, so the table leaves them out.

| Python | TypeScript | Reason |
| --- | --- | --- |
| `capture.py` | `capture.ts` | |
| `check-firmware-compatibility.py` | `check-firmware-compatibility.ts` | |
| `cia402-state.py` | `cia402-state.ts` | |
| `delete-file.py` | `delete-file.ts` | |
| `device-locate.py` | `device-locate.ts` | |
| `diagnostics.py` | `diagnostics.ts` | |
| `dump-process-data.py` | `dump-process-data.ts` | |
| `engage-brake.py` | `engage-brake.ts` | |
| `export-eni.py` | `export-eni.ts` | |
| `firmware-installation.py` | `firmware-installation.ts` | |
| `list-files.py` | `list-files.ts` | |
| `monitoring.py` | `monitoring.ts` | |
| `notifications.py` | `notifications.ts` | |
| `offset-detection.py` | `offset-detection.ts` | |
| `parse-esi.py` | `parse-esi.ts` | |
| `plant-identification.py` | `plant-identification.ts` | |
| `read-all-parameters.py` | `read-all-parameters.ts` | |
| `read-file.py` | `read-file.ts` | |
| `read-hardware-description.py` | `read-hardware-description.ts` | |
| `read-parameter.py` | `read-parameter.ts` | |
| `read-pdo-mapping.py` | `read-pdo-mapping.ts` | |
| `read-register.py` | `read-register.ts` | |
| `read-sdo.py` | `read-sdo.ts` | |
| `read-sii.py` | `read-sii.ts` | |
| `release-brake.py` | `release-brake.ts` | |
| `reset.py` | `reset.ts` | |
| `restart-devices.py` | `restart-devices.ts` | |
| `restore-default-parameters.py` | `restore-default-parameters.ts` | |
| `run-procedure.py` | `run-procedure.ts` | |
| `scan.py` | `scan.ts` | |
| `server-info.py` | `server-info.ts` | |
| `set-game-loop-period.py` | `set-game-loop-period.ts` | |
| `set-target.py` | `set-target.ts` | |
| `store-parameters.py` | `store-parameters.ts` | |
| `stream-monitoring.py` | `stream-monitoring.ts` | |
| `system-identification.py` | `system-identification.ts` | |
| `transition-state.py` | `transition-state.ts` | |
| `write-file.py` | `write-file.ts` | |
| `write-parameter.py` | `write-parameter.ts` | |
| `write-pdo-mapping.py` | `write-pdo-mapping.ts` | |
| `write-register.py` | `write-register.ts` | |
| `write-sdo.py` | `write-sdo.ts` | |
| `write-sii.py` | `write-sii.ts` | |
