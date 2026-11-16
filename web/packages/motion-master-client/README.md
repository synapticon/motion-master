# @synapticon/motion-master-client

The TypeScript client for [Motion Master](https://github.com/synapticon/motion-master), the motion control software for SOMANET servo drives. It runs in a browser and in Node.js.

The package contains four parts:

- `client.api`: an HTTP client for every operation of the Motion Master API. It is generated from the OpenAPI specification of the server.
- `client.ws`: a connection to the WebSocket of the server. It delivers monitoring data and notifications.
- The helper methods of the client, for the steps that most programs take. [Helpers](#helpers) lists them.
- Free functions: a parser for process data dumps (`.mmpd` files), CoE value encoding and decoding, `integerFromBytes`, `parseInteger` (the rule of Python's `int(text, 0)`), hex text with `formatHex` and `formatHexBytes`, object names with `objectName` and `objectAddressText`, the parameters of a device with `deviceParameterText` (`1:0x6041:00`) and `deviceParameterName` (`1:STATUSWORD`), the access word of an object with `objectAccess`, durations, the times of a request and byte counts with `formatDuration`, `formatTimes` and `formatBytes`, the default host and ports with `DEFAULT_HOST`, `DEFAULT_HTTP_PORT` and `DEFAULT_WS_PORT`, and the host names for a server on another machine.

## Version

The client and the server share one version number. A client works with the server of the same version. Install the client version that matches your server:

```bash
curl https://local.motion-master.synapticon.com:61447/api/version
npm install @synapticon/motion-master-client@6.0.0-alpha.121
```

Replace `6.0.0-alpha.121` with the version that the first command prints.

While Motion Master is in alpha, the API changes without notice between versions.

## Requirements

- Node.js 22 or later, or a current browser. The client uses the global `fetch` and `WebSocket`. Node.js has a global `WebSocket` from version 22.
- An ES module project. The package is ESM only.

A CommonJS project can load the package in two ways:

- `require('@synapticon/motion-master-client')` works from Node.js 22.12. Node.js 22 prints an `ExperimentalWarning` for it.
- `await import('@synapticon/motion-master-client')` works in every supported Node.js version.

## Quick start

```ts
import { MotionMasterClient } from '@synapticon/motion-master-client';

const client = new MotionMasterClient();

const { data } = await client.api.getVersion();
console.log(data.version);

client.close();
```

Without options, the client connects to a server on the same machine:

| Option | Default |
| --- | --- |
| `baseUrl` | `https://local.motion-master.synapticon.com:61447` |
| `wsUrl` | `wss://local.motion-master.synapticon.com:62281` |

The name `local.motion-master.synapticon.com` resolves to `127.0.0.1` in public DNS. The certificate that ships with the server covers it, so no setup is necessary.

## Settings from a file

In Node.js, `MotionMasterClient.fromConfig()` builds a client from `config.jsonc`. Copy the template and edit it:

```bash
cp config.example.jsonc config.jsonc
```

```ts
const client = await MotionMasterClient.fromConfig();
```

Without `path`, `fromConfig({ path })` reads `config.jsonc` in the current directory, and else `config.jsonc` in the folder of the package. The file is JSON with comments, like the configuration of the server, and the template explains each key. It has these keys. They are the keys of `config.toml` of the Python client, in camelCase:

| Key | Value |
| --- | --- |
| `host` | The host of the server, for example `local.motion-master.synapticon.com`. |
| `httpPort` | The port of the HTTP API, 61447. |
| `wsPort` | The port of the WebSocket, 62281. |
| `verify` | `true` checks the certificate of the server. `false` turns off the check for the whole process, because the built-in `fetch` and `WebSocket` of Node.js take no TLS option. |
| `fieldbus` | The body of `POST /api/init`: the driver, and the network adapter for SOEM or the IP addresses for SPoE. |

`config.jsonc` is ignored by git, so your adapter never gets committed. In a browser, pass the options to `new MotionMasterClient()` instead.

## Examples

The folder `examples/` holds one program per task, with the same name, options and output as the Python example of that task. Each program is a TypeScript file that runs with `tsx`. Create `config.jsonc` first, then run a program from the folder of the package:

```bash
pnpm install
pnpm example examples/server-info.ts
```

The examples need Node.js 22 or later. Check with `node --version`. On Fedora, `node` can stay at an older version after a system upgrade. This makes Node.js 22 the default:

```bash
sudo dnf install --allowerasing nodejs22-bin nodejs22-npm-bin
```

Each program prints what it does and its options with `--help`. It reads them with [commander](https://www.npmjs.com/package/commander), a dependency of the examples only: the package that npm installs does not need it. Three modules hold what the programs share, and are no examples of their own: `options.ts` (the command line), `procedures.ts` (`runToEnd`, which runs a procedure to its end and cancels it on Ctrl-C) and `files.ts` (the `files/` folder that the programs save into).

An example imports the package by its name, as a program that installed it from npm does. `examples/tsconfig.json` points that name to the source in `src/`, so the examples need no build. `pnpm typecheck:examples` checks them.

**The running server is shared.** A Console session that is connected to the same server sees every change that a program makes. A program that opens the driver, scans, resets or moves a device to another AL state changes what the Console shows.

The AL state (Application Layer state, the EtherCAT state machine of a device) column gives the lowest state the program needs. A program moves the device up to that state when the device is lower. A device that is already higher stays where it is. Three values in that column are not states:

- "Scanned" means that the program needs the devices known. It opens the driver and scans when needed, but it moves no device.
- "Driver open" means that the program opens the driver when none is open, and scans nothing.
- "No bus" means that the program opens no driver and leaves the server as it is.

The programs are the same as the Python examples, with the same names, options and output. `clients/PARITY.md` in the repository lists each pair.

### Server and bus

| Program | What it does | AL state |
| --- | --- | --- |
| `server-info.ts` | Prints the server version, the host, the network adapters and the health of the game loop. | No bus |
| `set-game-loop-period.ts` | Changes the cycle period of the real-time game loop until the server stops. | No bus |
| `notifications.ts` | Prints the notifications that the server sends over the WebSocket. | No bus |
| `scan.ts` | Scans the bus and lists the devices it finds. A scan leaves every device in INIT. | Scanned |
| `transition-state.ts` | Moves the devices to the AL state you give with `--state`, through every state between. | The state you give |
| `restart-devices.ts` | Restarts devices through INIT up to an AL state. An enabled drive is disabled. | Scanned |
| `reset.ts` | Closes the fieldbus driver and clears the device list. | No bus |
| `diagnostics.ts` | Prints the ESC (EtherCAT Slave Controller) error counters and the distributed clock sync of the devices. | Scanned |
| `capture.ts` | Records the EtherCAT frames on the bus adapter for some seconds, and prints how late the frames came back. | Driver open |

### Parameters

| Program | What it does | AL state |
| --- | --- | --- |
| `read-parameter.ts` | Reads one parameter with its name, data type and decoded value. `--cache` reads no bus. | PREOP |
| `write-parameter.ts` | Writes one parameter. It converts the value to the data type, and prints the value before and after. | PREOP |
| `read-all-parameters.ts` | Reads the whole object dictionary of the device with the value of every entry. | PREOP |
| `read-sdo.ts` | Reads one object with an SDO upload (Service Data Object, a mailbox read of one object dictionary entry), and prints its raw bytes. | PREOP |
| `write-sdo.ts` | Writes raw bytes to one object with an SDO download. | PREOP |
| `store-parameters.ts` | Stores the current parameter values in the flash of the device. | PREOP |
| `restore-default-parameters.ts` | Restores the default values of a parameter group. **Values not stored are gone.** | PREOP |

### Files and firmware

| Program | What it does | AL state |
| --- | --- | --- |
| `list-files.ts` | Lists the files stored on the device with their sizes. | PREOP |
| `read-file.ts` | Reads a file from the device over FoE (File access over EtherCAT), and saves it in `files/`. | PREOP |
| `write-file.ts` | Writes a local file to the device over FoE. A file of the same name is replaced. | PREOP |
| `delete-file.ts` | Deletes a file from the flash of a SOMANET device. | PREOP |
| `read-hardware-description.ts` | Prints what a SOMANET device says it is: the device, the assembly and their components. | PREOP |
| `check-firmware-compatibility.ts` | Says whether a firmware package belongs on the device, from the package filename alone. It installs nothing. | PREOP |
| `firmware-installation.ts` | Uploads a firmware package to the server and installs it on the device. The device is not controlled while it is off OP. | Scanned |

### ESC registers and SII

| Program | What it does | AL state |
| --- | --- | --- |
| `read-register.ts` | Reads bytes from an ESC register. | Scanned |
| `write-register.ts` | Writes bytes to an ESC register. | Scanned |
| `read-sii.ts` | Prints the SII (Slave Information Interface, the configuration in the EEPROM of the device), or saves the raw image with `--output`. | Scanned |
| `write-sii.ts` | Writes a raw SII image to the EEPROM. **A wrong image can leave the device unidentifiable.** The device adopts it after a power cycle. | Scanned |

### Process data

| Program | What it does | AL state |
| --- | --- | --- |
| `read-pdo-mapping.ts` | Prints the PDO (Process Data Object) mapping of the device, with the bit offset and the name of each entry. | PREOP |
| `write-pdo-mapping.ts` | Writes the PDO mapping from a JSON file. It moves the device to PREOP for the write, and back to SAFEOP or OP. | PREOP for the write |
| `stream-monitoring.ts` | Creates a monitoring and prints its batches of process data from the WebSocket. | OP |
| `monitoring.ts` | Records a move in Cyclic Synchronous Velocity with `client.monitoring()`: 250 for 2 s, then 0. Prints the target, the actual velocity and the CiA402 state, and saves every cycle as a CSV file in `files/`. **The motor turns.** | OP |
| `dump-process-data.ts` | Saves the recorder of the server, every recorded cycle of process data, as a `.mmpd` file in `files/`. | SAFEOP |

### Drive

| Program | What it does | AL state |
| --- | --- | --- |
| `cia402-state.ts` | Prints the CiA402 state of the drive with its actual and target values. | PREOP |
| `set-target.ts` | Sets the position, velocity or torque target. **An enabled drive moves to it at once.** | PREOP |
| `release-brake.ts` | Releases the brake of a SOMANET drive. It releases only while the drive is in Operation Enabled, and stays released. | PREOP |
| `engage-brake.ts` | Engages the brake of a SOMANET drive. | PREOP |
| `device-locate.ts` | Blinks the LEDs of the device. Only SPoE offers it. | PREOP |

### Procedures

| Program | What it does | AL state |
| --- | --- | --- |
| `run-procedure.ts` | Lists the procedures of a device. With a name, it runs that procedure with parameters given as `name=value`, and prints each step with its result. `--describe` prints the description, caveats and parameters instead. | PREOP, or `--state` |
| `offset-detection.ts` | Runs the offset detection procedure on a SOMANET drive, and prints each step with its result. **It turns the motor and releases the brake.** Ctrl-C cancels the run. | OP |
| `system-identification.ts` | Configures the system identification chirp, and arms it with `--start`. **Once armed, it turns the motor.** | PREOP |
| `plant-identification.ts` | Identifies the plant model with a chirp, and writes `plant_model.csv`. **It turns the motor in both directions.** | OP |

### ESI and ENI

| Program | What it does | AL state |
| --- | --- | --- |
| `parse-esi.ts` | Parses an ESI (EtherCAT Slave Information) file and lists the devices it describes. | No bus |
| `export-eni.ts` | Saves the bus as an ENI (EtherCAT Network Information) file in `files/`, for another master to replay. | SAFEOP |

Each program brings the server to the state it needs, so no program has to run before another. Each one runs its `main()` through `run()`. An error a user can act on, such as a server that is not running, then ends the program with one line instead of a stack trace.

## Helpers

The client has helpers for the steps that most programs take. They have the same names and the same behaviour as the helpers of the Python client, in camelCase. A required argument is positional. An optional one goes into an options object, with the same name as in Python, in camelCase. `clients/PARITY.md` in the repository lists both sets.

- `init({ fieldbus })` opens the fieldbus driver, unless one is open already. Without `fieldbus`, it sends the settings of the client.
- `reset()` closes the fieldbus driver and forgets every device. The devices stay in their AL state.
- `scan()` scans the bus every time and returns the number of devices it found.
- `devices()` returns the scanned devices, in bus order.
- `parameters(slavePosition)` returns every parameter of a device, with its value and metadata.
- `hardwareDescription(slavePosition)` returns the parsed `.hardware_description` of a device.
- `readValue(slavePosition, address)` reads one object and returns the value that the server decoded. `writeValue(slavePosition, address, value)` writes one, converted by the server to its data type, and returns the value read back.
- `readSdo(slavePosition, index, subindex)` returns the raw bytes of one object, read with an SDO upload, and `writeSdo(slavePosition, index, subindex, bytes)` writes raw bytes with an SDO download. `integerFromBytes(bytes, { byteOrder, signed })` turns bytes into an integer of any length, little-endian by default.
- `listFiles(slavePosition)` returns the files on a device, each as `{ name, byteCount }`. `readFile(slavePosition, name, { format })` reads one over FoE as `'bytes'` (the default), `'text'` or `'json'`. `writeFile(slavePosition, name, content)` writes bytes, or a string as UTF-8. `removeFile(slavePosition, name)` removes a file.
- `readSii(slavePosition, { format })` returns the parsed SII of a device (`'json'`, the default) or its raw EEPROM image (`'bytes'`). `writeSii(slavePosition, image)` writes a raw image.
- `listUserCacheFiles()` returns the files in the user cache of the server, a folder that keeps files across restarts: its `root`, and each file with its `path` relative to the root, its `size` and its `modifiedMs`. `readUserCacheFile(path, { format })`, `writeUserCacheFile(path, content)` and `removeUserCacheFile(path)` read, write and remove one file, as `readFile`, `writeFile` and `removeFile` do on a device. The path has `/` between its folders.
- `readHrdRecording(slavePosition, data, { format })` reads the high resolution data recording that the `hrd-streaming` procedure left on a drive, decoded as `data` says (`'encoder-raw'` or `'system-identification'`). It returns the recording, or CSV text with `format: 'csv'`.
- `listParameterCacheEntries()`, `readParameterCacheEntry(id, { format })` and `removeParameterCacheEntry(id)` manage the parameter cache, the object dictionaries that a scan reads from the cache instead of from a device. `serverLog()` returns the log of the server as text.
- `runAutoTuning(request)` runs one function of the auto-tuning program of the server, such as `{ run: 'auto_tune_velocity_controller', data }`, and returns its result. The program answers an input that it refuses with an `error` property in the result.
- `parseSii(image)`, `parseIntegroVariant(bytes)` and `parseIcMuRestoreFile(bytes)` decode a file on the server, with no device involved. `parseEsi(xml, { modules })` reads an ESI document and returns every device it describes.
- `serverTimeMs(response)` returns the time that the server spent on the request, in milliseconds, from the metric `server` of the `Server-Timing` header. `fieldbusTimeMs(response)` returns the time of the fieldbus operation of the request, from the metric `fieldbus`. Both also read a `PerformanceResourceTiming` entry of the browser. They return `null` when the response has no such time.
- `waitForValue(slavePosition, address, predicate, { timeoutMs, intervalMs, signal })` reads one object until the predicate is true. It throws a `DOMException` named `TimeoutError` with the last value when the time runs out, 1000 ms by default.
- `ensureScanned()` scans when the last scan is missing or wrong, and moves no device.
- `ensureState(state, { positions, exact })` scans when needed and brings the devices to an AL state. A device above that state stays where it is, unless `exact` is true.
- `restart(target, { positions })` restarts devices through INIT and walks them up to an AL state.
- `setOperationMode(slavePosition, mode)` writes the CiA402 operation mode, such as `OperationMode.CYCLIC_SYNC_VELOCITY`, and returns when the drive shows it.
- `setTarget(slavePosition, kind, value)` writes the `'position'`, `'velocity'` or `'torque'` target, in the units of its object.
- `enable(slavePosition)` brings a drive to Operation Enabled. The device must exchange process data.
- `disable(slavePosition)` brings a drive to Switch On Disabled.
- `quickStop(slavePosition)` sends a quick stop. The drive stops on its quick stop ramp.
- `runProcedure(slavePosition, name, { params, pollIntervalMs, signal })` starts a procedure and yields its snapshot at every poll, 500 ms apart by default, until the run ends. Leaving the loop early, or an abort of `signal`, cancels the run. It throws a `ProcedureError` when the server refuses the start.
- `processDataMark()` returns the recorder clock of the server, in epoch microseconds. `processDataDump({ fromUs, toUs })` returns the `.mmpd` dump of the cycles between two marks as bytes, or of the whole recorder without them, and `parseMmpd()` decodes it. `processDataDump` returns `null` when the server has no cycle to dump, and throws a `ProcessDataDumpError` for any other refusal. `parseMmpd(bytes).header.flags` has `MMPD_FLAG_INCOMPLETE_START` set when the server no longer held every cycle right after `fromUs`.
- `exportEni()` returns the bus as an ENI document, `{ xml, warnings }`. `parseEni(xml)` reads an ENI document and returns its network with each datagram annotated.
- `startCapture({ adapter, seconds })` starts a capture of the EtherCAT frames that runs until `stopCapture()`, which returns the path of the pcap file in the user cache and the summary of late frames. Linux and macOS servers only. Both throw a `CaptureError` with the server's reason.
- `streamMonitoring(topic)`, `monitoring(parameters, span)` and `notifications()` read from the WebSocket. [Monitoring](#monitoring) and [Notifications](#notifications) describe them.
- `stepChanges(run)` yields each snapshot of a run as `[snapshot, changed]`, where `changed` lists the steps whose status changed. `formatStep(step)` turns a step into one line.
- `stepValue(snapshot, id)` returns what one step of a run produced, and `procedureFailure(snapshot)` why a run failed or was cancelled. `procedureParameterValue(parameter, text)` converts the text of one parameter by the type that the descriptor of the procedure declares, and throws an `Error` that says what is wrong with it. A list splits on commas and line breaks.

```ts
for await (const [snapshot, changed] of stepChanges(client.runProcedure(1, 'store-parameters'))) {
  for (const step of changed) {
    console.log(formatStep(step));
  }
}
```

`errorLine(error)` returns one line for an error a user can act on, such as `POST /api/devices/state/transition: HTTP 409, …` or `cannot connect to …`, and `null` for a bug. `run(main)` calls `main` in Node.js and ends the program with that line and exit code 1, while a bug keeps its stack trace. Set `MM_TRACEBACK=1` to get the stack trace for every error.

`ensureScanned`, `ensureState` and `restart` throw a `StateError` when a device does not answer or does not reach its state. A failed HTTP request throws an `HttpError`, and a server that the client cannot reach throws a `ConnectionError`.

The helpers log each step they take, such as `found 1 device(s)`, through the [`debug`](https://www.npmjs.com/package/debug) package. The log is off by default. To turn it on in Node.js, set the environment variable:

```bash
DEBUG=motion-master-client pnpm example examples/scan.ts
```

In a browser, run `localStorage.debug = 'motion-master-client'` in the developer tools and reload the page.

The namespace `motion-master-client:http` writes one line per HTTP request: the method, the path, the status, the round trip, and the times that the server reported in `Server-Timing`: `server`, and `fieldbus` for a request that ran a fieldbus operation. `DEBUG=motion-master-client*` turns on both logs.

```bash
DEBUG=motion-master-client:http pnpm example examples/server-info.ts
```

```text
motion-master-client:http GET /api/version 200 round-trip 0.912 ms, server 0.087 ms
motion-master-client:http GET /api/devices/1/sdo/0x1000/0 200 round-trip 1.733 ms, server 1.292 ms, fieldbus 1.133 ms
```

A helper returns the decoded result. A program that needs a response, for its headers, calls the method of `client.api`, which returns it, as the next section describes. Every operation can go through `client.api`, including those that send a file.

## HTTP calls

Each operation of the API is a method of `client.api`. The method name is the `operationId` of the operation in the OpenAPI specification. A method returns a promise of a response object. The response body is in `data`.

```ts
const { data: devices } = await client.api.getDevices();

const { data: value } = await client.api.readParameter(1, 0x6064, 0);
```

Every request of the client, `client.api` included, throws an `Error` when it fails:

- `HttpError` when the server refuses the request. It has `request`, `response`, `status` and `reason`, the reason the server gives. `response` has its body unread.
- `ConnectionError` when the request does not reach the server. It has `request`, and `cause` is the error of `fetch`.

`apiErrorMessage(error)` returns the reason of an `HttpError` and the message of any other error, for a page to show:

```ts
import { apiErrorMessage, HttpError } from '@synapticon/motion-master-client';

try {
  await client.api.scan();
} catch (err) {
  if (err instanceof HttpError && err.status === 409) {
    // The bus is busy.
  }
  console.error(apiErrorMessage(err));
}
```

The server serves its OpenAPI specification at `GET /api/swagger.yml`. The Motion Master Console shows the same specification under API Docs. Your editor also shows the types and the descriptions of every method.

## Objects and states

`Profile`, `Cia402` and `Somanet` name every object of a SOMANET device as an address: an index, a subindex and the type of the value. `Profile` holds the CiA 301 communication area (0x1xxx) and the standard MDP objects, `Cia402` the CiA402 drive profile (0x6xxx), and `Somanet` the SOMANET vendor area (0x2xxx) and FSoE. The names are the same as in the Python client, and the C++ names of the server without their `k`, in capitals. `readValue` takes an address and returns a value of its type:

```ts
import { AlState, Cia402, Somanet } from '@synapticon/motion-master-client';

const statusword = await client.readValue(1, Cia402.STATUSWORD); // number
const state = Cia402.state(statusword); // 'OperationEnabled', ...
const temperature = await client.readValue(1, Somanet.CORE_TEMPERATURE_MEASURED_TEMPERATURE);

await client.api.transitionToState({ state: AlState.PREOP });
```

The list holds every object of every device in the ESI, so an address can name an object that the drive does not have. A read of it then fails. A 64-bit value is exact only up to 2^53, because JavaScript reads a JSON integer as a `number`.

`AlState` names the AL states, `INIT`, `PREOP`, `BOOT`, `SAFEOP` and `OP`, with the numbers that the API uses. The server reports `UNKNOWN`, 0, for a device that does not answer. `alStateName(2)` returns `PREOP`, and `mailboxActive(alState)` says whether a device in that state answers mailbox requests, such as an SDO: in PREOP, SAFEOP and OP. `Cia402State` is the type of a CiA402 state name. `Cia402.state()` returns the state that a statusword reports, which is useful for a recorded row, where the server sends the raw statusword.

## Monitoring

A monitoring is a list of objects that the server samples in every cycle and sends over the WebSocket. You create it over HTTP, then you subscribe to its topic.

```ts
await client.api.createMonitoring({
  topic: 'my-axis',
  interval: 100,
  parameters: [
    [1, 0x6041, 0], // device position, index, subindex
    [1, 0x6064, 0],
  ],
});

for await (const rows of client.streamMonitoring('my-axis')) {
  for (const [timestampUs, ...values] of rows) {
    console.log(timestampUs, values);
  }
  if (done()) {
    break; // leaving the loop unsubscribes
  }
}
await client.api.deleteMonitoring('my-axis');
```

To record a monitoring for a span and read its rows afterwards, use `monitoring()`. It creates the monitoring, waits for the first row, runs the span, waits for the last flush, and deletes the monitoring, also when the span throws:

```ts
const m = await client.monitoring([[1, Cia402.VELOCITY_ACTUAL_VALUE]], async () => {
  // the move
});
const velocity = m.column(1, Cia402.VELOCITY_ACTUAL_VALUE); // [timestampUs, value][]
```

`monitoring()` throws a `MonitoringError` when the server refuses the monitoring, when no row arrives within 5000 ms, when the rows of the last flush do not arrive, and when the WebSocket closes during the recording. An error of the span wins over them. Pass `signal` to stop the waits on an abort, and pass the same signal to what the span waits for. In Node.js, Ctrl-C ends a program at once and runs no `finally` block, so a program that moves a motor aborts the signal on `SIGINT`, as `examples/monitoring.ts` does.

`intervalMs` is the flush interval, 50 when not given. The result is a `MonitoringRecording`:

- `column()` returns the values of one object with their timestamps.
- `values()` returns the values in a time window.
- `first()` returns the time of the first value that meets a condition.
- `printTable()` prints one row every `everyMs`, with the CiA402 state beside a statusword.
- `toCsv()` returns every row as CSV text, such as for a download in a browser. `writeCsv(path)` writes it to a file, in Node.js.
- `names` gives the name of each column, such as `1:STATUSWORD`, which `printTable()` and `writeCsv()` use. `sources` gives where each value comes from, `pdo` or `sdo`.

A parameter is `[devicePosition, index, subindex]` or `[devicePosition, address]`.

`streamMonitoring` yields the batches of a monitoring that exists already. To get each batch in a callback instead, use `client.ws.subscribe(topic, listener)`, which returns the function that unsubscribes.

A stream of `streamMonitoring()` or `notifications()` takes a `signal`, and an abort ends the loop with its reason, also while the loop waits. `client.close()` ends the loop. Any other close of the WebSocket throws a `ConnectionError`, because the messages sent while the socket was closed are lost.

Each batch holds one row per cycle since the last batch. A row is the time in microseconds since the Unix epoch, then one value for each object, in the order of `parameters`. `GET /api/monitorings/{topic}` returns that order. A value is `null` while its device does not exchange process data, which starts in SAFEOP.

`interval` is the time in milliseconds between two batches, from 5 to 2000. It does not change the sample rate. A longer interval gives larger batches.

The server keeps a monitoring until you delete it. A second monitoring with the same topic fails with 409.

The section "Monitoring WebSocket" in the description of the OpenAPI specification gives the full message format, including the type of each value and the fields of each notification.

## Notifications

The server sends events, such as a short working counter on the bus, as notifications. One subscription receives every kind of event, also a kind that a later server adds:

```ts
for await (const data of client.notifications()) {
  console.log(data.event, data);
}
```

The `event` field of `data` gives the kind of event. Leaving the loop unsubscribes. To get each notification in a callback instead, use `client.ws.onNotification(listener)`, which returns the function that unsubscribes.

## The WebSocket connection

The client opens the WebSocket when you first subscribe, and closes it when the last subscription ends, so that a Node.js program can end. `client.ws.connect()` keeps it open until `client.close()`. If the connection drops, the client connects again and subscribes again to every topic. A stream of `streamMonitoring()` or `notifications()` throws on the drop instead.

`client.ws.onStateChange` reports the state: `connecting`, `open` or `closed`. `client.close()` closes the WebSocket. The HTTP client needs no close.

## A server on another machine

The certificate of the server covers names under `ip.motion-master.synapticon.com`. Such a name holds the IP address with dashes in place of the dots. `lanHostname` makes the name from the address:

```ts
import { MotionMasterClient, lanHostname } from '@synapticon/motion-master-client';

const host = lanHostname('192.168.1.50'); // 192-168-1-50.ip.motion-master.synapticon.com
const client = new MotionMasterClient({
  baseUrl: `https://${host}:61447`,
  wsUrl: `wss://${host}:62281`,
});
```

These names are not in public DNS. Add one line to the hosts file of the machine that runs the client:

```text
192.168.1.50 192-168-1-50.ip.motion-master.synapticon.com
```

The server must also listen on an address other than `127.0.0.1`. The `server.bindAddress` setting sets that address. See [LAN deployment](https://github.com/synapticon/motion-master/blob/main/docs/LAN_DEPLOYMENT.md).

## Use in a browser

A browser sends an HTTP request to another origin only when the server allows that origin. The server allows one origin, the `server.corsOrigin` setting in its configuration file, `motion-master.jsonc`. The default is `https://motion-master.synapticon.com`, the hosted Console. Set it to the origin of your application, for example `http://localhost:5173` for a Vite development server:

```jsonc
{ "server": { "corsOrigin": "http://localhost:5173" } }
```

Restart the server after the change.

## Use in Node.js

The server certificate is valid, so Node.js connects with no options. A server with a self-signed certificate, for example a development build, needs the certificate check turned off. Set `"verify": false` in `config.jsonc`, or start Node.js with `NODE_TLS_REJECT_UNAUTHORIZED=0`. Both turn off certificate checks for the whole process, for the HTTP requests and the WebSocket. Use this only against a development server.

To turn off the check for the WebSocket only, inject the `ws` package:

```ts
import WebSocket from 'ws';

const client = new MotionMasterClient({
  WebSocket,
  webSocketOptions: { rejectUnauthorized: false },
});
```

## Licence

GPL v3, the licence of the whole Motion Master repository. See `LICENSE`.
