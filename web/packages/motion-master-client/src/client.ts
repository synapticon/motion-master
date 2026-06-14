// The MotionMasterClient facade: a single connection object that composes the generated HTTP API
// (`client.api.*`, flat — e.g. `client.api.getVersion()`) with the WebSocket connection
// (`client.ws`). It is a *connection, not a model*: it holds no mutable device/parameter/AL state
// — the HTTP API and the monitoring stream are the source of truth. Isomorphic: pass `WebSocket`
// (and `customFetch`) to run under Node; both default to the platform globals in the browser.

import { Api } from './generated/Api'
import type { ApiConfig } from './generated/http-client'
import { API_BASE_URL, WS_URL } from './constants'
import { fetchProcessDataDump, type MmpdFile } from './mmpd'
import { WebSocketConnection, type WebSocketConstructor } from './web-socket-connection'

export interface MotionMasterClientOptions {
  /// Origin (or `host:port`) of the HTTP API. Default: the bundled localhost dev origin.
  baseUrl?: string
  /// Full URL of the WebSocket connection. Default: the bundled localhost dev origin.
  wsUrl?: string
  /// WebSocket implementation for `client.ws`. Defaults to the global `WebSocket` (browsers,
  /// Node >= 22); inject the `ws` package's constructor on older Node.
  WebSocket?: WebSocketConstructor
  /// Second argument for the WebSocket constructor — e.g. `{ rejectUnauthorized: false }` for the
  /// Node `ws` client against a self-signed dev cert. Ignored by the browser global.
  webSocketOptions?: unknown
  /// Custom `fetch` forwarded to the HTTP client (e.g. request logging in tests).
  customFetch?: ApiConfig['customFetch']
}

export class MotionMasterClient {
  /// The generated HTTP API client. Flat method surface: `client.api.getVersion()`,
  /// `client.api.scan()`, `client.api.sdoUpload(...)`, ...
  readonly api: Api

  /// The WebSocket connection: `client.ws.subscribe(topic, cb)`,
  /// `client.ws.onNotification(cb)`, `client.ws.onProgress(cb)`. Connects lazily.
  readonly ws: WebSocketConnection

  private readonly httpBaseUrl: string
  private readonly customFetch?: ApiConfig['customFetch']

  constructor(options: MotionMasterClientOptions = {}) {
    this.httpBaseUrl = options.baseUrl ?? API_BASE_URL
    this.customFetch = options.customFetch
    this.api = new Api({ baseUrl: this.httpBaseUrl, customFetch: options.customFetch })
    this.ws = new WebSocketConnection({
      url: options.wsUrl ?? WS_URL,
      WebSocket: options.WebSocket,
      protocolsOrOptions: options.webSocketOptions,
    })
  }

  /// Fetches and parses the live recorder dump (`GET /api/process-data/dump`) as an `MmpdFile`.
  fetchProcessDataDump(signal?: AbortSignal): Promise<MmpdFile> {
    return fetchProcessDataDump(this.httpBaseUrl, { fetch: this.customFetch, signal })
  }

  /// Closes the WebSocket connection (and cancels reconnection). The HTTP API needs no teardown.
  close(): void {
    this.ws.close()
  }
}
