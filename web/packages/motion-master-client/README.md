# @synapticon/motion-master-client

The TypeScript client for [Motion Master](https://github.com/synapticon/motion-master), the motion control software for SOMANET servo drives. It runs in a browser and in Node.js.

The package contains three parts:

- `client.api`: an HTTP client for every operation of the Motion Master API. It is generated from the OpenAPI specification of the server.
- `client.ws`: a connection to the WebSocket of the server. It delivers monitoring data and notifications.
- Helpers: a parser for process data dumps (`.mmpd` files), CoE value encoding and decoding, ENI export and parse, and the host names for a server on another machine.

The licence is MIT.

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
import { MotionMasterClient } from '@synapticon/motion-master-client'

const client = new MotionMasterClient()

const { data } = await client.api.getVersion()
console.log(data.version)

client.close()
```

Without options, the client connects to a server on the same machine:

| Option | Default |
| --- | --- |
| `baseUrl` | `https://local.motion-master.synapticon.com:61447` |
| `wsUrl` | `wss://local.motion-master.synapticon.com:62281` |

The name `local.motion-master.synapticon.com` resolves to `127.0.0.1` in public DNS. The certificate that ships with the server covers it, so no setup is necessary.

## HTTP calls

Each operation of the API is a method of `client.api`. The method name is the `operationId` of the operation in the OpenAPI specification. A method returns a promise of a response object. The response body is in `data`.

```ts
const { data: devices } = await client.api.getDevices()

const { data: value } = await client.api.readParameter(1, 0x6064, 0)
```

A request that fails rejects with the response object, not with an `Error`. The server puts its reason in the body. `apiErrorMessage` gets that reason from any rejected value:

```ts
import { apiErrorMessage } from '@synapticon/motion-master-client'

try {
  await client.api.scan()
} catch (err) {
  console.error(apiErrorMessage(err))
}
```

The server serves its OpenAPI specification at `GET /api/swagger.yml`. The Motion Master Console shows the same specification under API Docs. Your editor also shows the types and the descriptions of every method.

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
})

const unsubscribe = client.ws.subscribe('my-axis', (rows) => {
  for (const [timestampUs, ...values] of rows) {
    console.log(timestampUs, values)
  }
})

// Later:
unsubscribe()
await client.api.deleteMonitoring('my-axis')
```

Each batch holds one row per cycle since the last batch. A row is the time in microseconds since the Unix epoch, then one value for each object, in the order of `parameters`. `GET /api/monitorings/{topic}` returns that order. A value is `null` while its device does not exchange process data, which starts in SAFE-OP.

`interval` is the time in milliseconds between two batches, from 5 to 2000. It does not change the sample rate. A longer interval gives larger batches.

The server keeps a monitoring until you delete it. A second monitoring with the same topic fails with 409.

The section "Monitoring WebSocket" in the description of the OpenAPI specification gives the full message format, including the type of each value and the fields of each notification.

## Notifications

The server sends events, such as a short working counter on the bus, as notifications. One listener receives every kind of event:

```ts
const stop = client.ws.onNotification((data) => {
  console.log(data)
})
```

The `event` field of `data` gives the kind of event.

## The WebSocket connection

The client opens the WebSocket when you first subscribe. If the connection drops, the client connects again and subscribes again to every topic.

`client.ws.onStateChange` reports the state: `connecting`, `open` or `closed`. `client.close()` closes the WebSocket. The HTTP client needs no close.

## A server on another machine

The certificate of the server covers names under `ip.motion-master.synapticon.com`. Such a name holds the IP address with dashes in place of the dots. `lanHostname` makes the name from the address:

```ts
import { MotionMasterClient, lanHostname } from '@synapticon/motion-master-client'

const host = lanHostname('192.168.1.50') // 192-168-1-50.ip.motion-master.synapticon.com
const client = new MotionMasterClient({
  baseUrl: `https://${host}:61447`,
  wsUrl: `wss://${host}:62281`,
})
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

The server certificate is valid, so Node.js connects with no options. A server with a self-signed certificate, for example a development build, needs two changes:

- For HTTP, start Node.js with `NODE_TLS_REJECT_UNAUTHORIZED=0`. This turns off certificate checks for the whole process. Use it only against a development server.
- For the WebSocket, inject the `ws` package and turn off its certificate check:

```ts
import WebSocket from 'ws'

const client = new MotionMasterClient({
  WebSocket,
  webSocketOptions: { rejectUnauthorized: false },
})
```

## Licence

MIT. See `LICENSE`.
