# @synapticon/motion-master-client

The TypeScript client for [Motion Master](https://github.com/synapticon/motion-master), the motion control software for SOMANET servo drives. It runs in a browser and in Node.js.

The package contains three parts:

- `client.api`: an HTTP client for every operation of the Motion Master API. It is generated from the OpenAPI specification of the server.
- `client.ws`: a connection to the WebSocket of the server. It delivers monitoring data and notifications.
- Helpers: a parser for process data dumps (`.mmpd` files), CoE value encoding and decoding, ENI export and parse, and the host names for a server on another machine.

The licence is GPL v3, the licence of the whole Motion Master repository.

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

The folder `examples/` holds one program per task, as the Python client does. Each program is a TypeScript file that runs in Node.js 22 with `tsx`. Create `config.jsonc` first, then run a program from the folder of the package:

```bash
pnpm install
pnpm example examples/server-info.ts
```

The examples that use the WebSocket, such as `monitoring.ts`, need Node.js 22, because older versions have no global `WebSocket`. Check with `node --version`. On Fedora, `node` can stay at an older version after a system upgrade. This makes Node.js 22 the default:

```bash
sudo dnf install --allowerasing nodejs22-bin nodejs22-npm-bin
```

An example imports the package by its name, as a program that installed it from npm does. `examples/tsconfig.json` points that name to the source in `src/`, so the examples need no build. `pnpm typecheck:examples` checks them.

| Program | What it does |
| --- | --- |
| `server-info.ts` | Prints the server version, the host, the network adapters and the game loop health. It opens no fieldbus driver. |
| `scan.ts` | Scans the bus and lists the devices with their AL state. |
| `reset.ts` | Closes the fieldbus driver and clears the device list. |
| `read-parameter.ts` | Reads one parameter with its name, data type and value. `--cache` touches no bus. |
| `read-hardware-description.ts` | Prints what a device says it is: its name, build descriptor, serial number and components. |
| `transition-state.ts` | Moves the devices to an AL state, for example `--state op`. |
| `restart-devices.ts` | Restarts devices through INIT up to an AL state. |
| `cia402-state.ts` | Prints the CiA 402 state of a drive, with its actual and target values. |
| `set-target.ts` | Sets the position, velocity or torque target of a drive. **A drive that is enabled moves.** |
| `run-procedure.ts` | Lists the procedures of a device, describes one with `--describe`, or runs one by name with its parameters as `name=value`. Ctrl-C cancels the run. |
| `device-locate.ts` | Blinks the LEDs of a device. Only SPoE offers the procedure. |
| `dump-process-data.ts` | Saves the recorder of the server as a `.mmpd` file in `files/`. |
| `export-eni.ts` | Saves the bus as an ENI (EtherCAT Network Information) file in `files/`, for another EtherCAT master. |
| `capture.ts` | Records the EtherCAT frames on the bus adapter for `--seconds` and prints the summary of late frames. Linux and macOS servers only. |
| `notifications.ts` | Prints the notifications of the server until Ctrl-C, or `--count` of them. It opens no fieldbus driver. |
| `monitoring.ts` | Records a move in Cyclic Synchronous Velocity with `client.monitoring()`, prints it as a table, and saves every cycle as CSV in `files/`. **The motor turns.** |

Each program opens the fieldbus driver, scans and moves a device to the AL state it needs, so no program has to run before another. Each one runs its `main()` through `run()`, so an error a user can act on, such as a server that is not running, ends the program with one line instead of a stack trace.

## Helpers

The client has helpers for the steps that most programs begin with. They have the same names and the same behaviour as the helpers of the Python client, in camelCase. A required argument is positional. An optional one goes into an options object, with the name it has in Python. `clients/PARITY.md` in the repository lists both sets.

- `init({ fieldbus })` opens the fieldbus driver, unless one is open already. Without `fieldbus`, it sends the settings of the client.
- `reset()` closes the fieldbus driver and forgets every device. The devices stay in their AL state.
- `scan()` scans the bus every time and returns the number of devices it found.
- `devices()` returns the scanned devices, in bus order.
- `parameters(slavePosition)` returns every parameter of a device, with its value and metadata.
- `hardwareDescription(slavePosition)` returns the parsed `.hardware_description` of a device.
- `readValue(slavePosition, address)` reads one object and returns the value that the server decoded.
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
- `processDataMark()` returns the recorder clock of the server, in epoch microseconds. `processDataDump({ fromUs, toUs })` returns the `.mmpd` dump of the cycles between two marks as bytes, or of the whole recorder without them, and `parseMmpd()` decodes it. It returns `null` when the server has no cycle to dump, and throws a `ProcessDataDumpError` for any other refusal.
- `exportEni()` returns the bus as an ENI document, `{ xml, warnings }`. `parseEni(xml)` reads an ENI document and returns its network with each datagram annotated.
- `startCapture({ adapter, seconds })` starts a capture of the EtherCAT frames that runs until `stopCapture()`, which returns the path of the pcap file in the user cache and the summary of late frames. Linux and macOS servers only. Both throw a `CaptureError` with the server's reason.
- `streamMonitoring(topic)`, `monitoring(parameters, span)` and `notifications()` read from the WebSocket. [Monitoring](#monitoring) and [Notifications](#notifications) describe them.
- `stepChanges(run)` yields each snapshot of a run as `[snapshot, changed]`, where `changed` lists the steps whose status changed. `formatStep(step)` turns a step into one line.

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

`Profile`, `Cia402` and `Somanet` name every object of a SOMANET device as an address: an index, a subindex and the type of the value. `Profile` holds the CiA 301 communication area (0x1xxx) and the standard MDP objects, `Cia402` the CiA 402 drive profile (0x6xxx), and `Somanet` the SOMANET vendor area (0x2xxx) and FSoE. The names are the same as in the Python client, and the C++ names of the server without their `k`, in capitals. `readValue` takes an address and returns a value of its type:

```ts
import { AlState, Cia402, Somanet } from '@synapticon/motion-master-client';

const statusword = await client.readValue(1, Cia402.STATUSWORD); // number
const state = Cia402.state(statusword); // 'OperationEnabled', ...
const temperature = await client.readValue(1, Somanet.CORE_TEMPERATURE_MEASURED_TEMPERATURE);

await client.api.transitionToState({ state: AlState.PREOP });
```

The list holds every object of every device in the ESI, so an address can name an object that the drive does not have. A read of it then fails. A 64-bit value is exact only up to 2^53, because JavaScript reads a JSON integer as a `number`.

`AlState` names the AL states, `INIT`, `PREOP`, `BOOT`, `SAFEOP` and `OP`, with the numbers that the API uses. The server reports `UNKNOWN`, 0, for a device that does not answer. `Cia402State` is the type of a CiA402 state name. `Cia402.state()` returns the state that a statusword reports, which is useful for a recorded row, where the server sends the raw statusword.

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

The result is a `MonitoringRecording`. `column()` returns the values of one object with their timestamps, `values()` the values in a time window, and `first()` the time of the first value that meets a condition. `printTable()` prints one row every `everyMs`, with the CiA402 state beside a statusword, and `writeCsv(path)` writes every row to a CSV file in Node.js. Both name a column like `1:STATUSWORD`. A parameter is `[devicePosition, index, subindex]` or `[devicePosition, address]`.

`streamMonitoring` yields the batches of a monitoring that exists already. To get each batch in a callback instead, use `client.ws.subscribe(topic, listener)`, which returns the function that unsubscribes.

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

The client opens the WebSocket when you first subscribe. If the connection drops, the client connects again and subscribes again to every topic.

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
