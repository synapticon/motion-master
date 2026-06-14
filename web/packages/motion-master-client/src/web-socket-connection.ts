// Framework-agnostic connection to Motion Master's single WebSocket — the one bidirectional
// socket that carries server->client monitoring batches, notifications, and (planned) procedure
// progress, and client->server topic subscribe/unsubscribe. It owns reconnection, multiplexes
// per-topic subscriptions over the one socket (ref-counted: subscribe on the first listener,
// unsubscribe on the last, re-subscribe everything on reconnect), and routes inbound frames by
// type. It is isomorphic: the browser global `WebSocket` is used by default, and any compatible
// implementation (e.g. the Node `ws` package) can be injected — there is no hard dependency on
// either, so the package stays usable in both environments.

/// Connection state, mirroring the three states the UI cares about.
export type ReadyState = 'connecting' | 'open' | 'closed'

/// One published monitoring batch for a topic: rows of `[timestampUs, v0, v1, ...]` — epoch
/// microseconds followed by one value per parameter (positionally ordered; fetch the order once
/// from `GET /api/monitorings/{topic}`). A value is `null` when its device was not exchanging at
/// sample time. The stream is lossless: one row per recorded cycle since the last flush.
export type MonitoringRow = (number | null)[]
export type MonitoringBatch = MonitoringRow[]

export interface MonitoringMessage {
  type: 'monitoring'
  topic: string
  data: MonitoringBatch
}

export interface NotificationMessage {
  type: 'notification'
  data: unknown
}

/// Procedure progress — reserved on the server, not yet on the wire. Routed already so callers can
/// register handlers today and start receiving the moment the backend publishes them.
export interface ProgressMessage {
  type: 'progress'
  data: unknown
}

export type ServerMessage = MonitoringMessage | NotificationMessage | ProgressMessage

type MonitoringListener = (rows: MonitoringBatch) => void
type NotificationListener = (data: unknown) => void
type ProgressListener = (data: unknown) => void
type StateListener = (state: ReadyState) => void

/// The numeric `readyState` value a WebSocket reports when it is open. Hard-coded (rather than read
/// off a global) so the client never depends on a particular `WebSocket` implementation's statics.
const WS_OPEN = 1

/// Minimal structural shape of a WebSocket instance — satisfied by both the browser global and the
/// Node `ws` package (which implements the same `on*` property setters) — so neither is a hard
/// dependency.
export interface WebSocketLike {
  readyState: number
  send(data: string): void
  close(): void
  onopen: ((event: unknown) => void) | null
  onclose: ((event: unknown) => void) | null
  onerror: ((event: unknown) => void) | null
  onmessage: ((event: { data: unknown }) => void) | null
}

export type WebSocketConstructor = new (
  url: string,
  protocolsOrOptions?: unknown,
) => WebSocketLike

export interface WebSocketConnectionOptions {
  /// Full WebSocket URL (e.g. `wss://host:62281`).
  url: string
  /// WebSocket implementation. Defaults to the global `WebSocket` (browsers, Node >= 22). In older
  /// Node, inject the `ws` package's constructor.
  WebSocket?: WebSocketConstructor
  /// Extra argument forwarded as the constructor's second parameter — e.g.
  /// `{ rejectUnauthorized: false }` for the Node `ws` client against a self-signed dev cert. The
  /// browser global ignores it.
  protocolsOrOptions?: unknown
  /// Base reconnect backoff in ms (doubled each consecutive failure up to `maxReconnectDelayMs`).
  /// Set to 0 to disable auto-reconnect. Default 1000.
  reconnectDelayMs?: number
  /// Upper bound on the reconnect backoff in ms. Default 15000.
  maxReconnectDelayMs?: number
}

export class WebSocketConnection {
  private readonly url: string
  private readonly WebSocketImpl?: WebSocketConstructor
  private readonly protocolsOrOptions?: unknown
  private readonly reconnectDelayMs: number
  private readonly maxReconnectDelayMs: number

  private socket: WebSocketLike | null = null
  private state: ReadyState = 'closed'
  private closedByUser = false
  private reconnectAttempts = 0
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null

  private readonly topicListeners = new Map<string, Set<MonitoringListener>>()
  private readonly notificationListeners = new Set<NotificationListener>()
  private readonly progressListeners = new Set<ProgressListener>()
  private readonly stateListeners = new Set<StateListener>()

  constructor(options: WebSocketConnectionOptions) {
    this.url = options.url
    this.WebSocketImpl = options.WebSocket
    this.protocolsOrOptions = options.protocolsOrOptions
    this.reconnectDelayMs = options.reconnectDelayMs ?? 1000
    this.maxReconnectDelayMs = options.maxReconnectDelayMs ?? 15000
  }

  get readyState(): ReadyState {
    return this.state
  }

  /// Opens the socket if it is not already open or connecting. Idempotent — safe to call from every
  /// subscribe path. Connection is lazy: an HTTP-only consumer that never subscribes never opens a
  /// socket.
  connect(): void {
    if (this.socket || this.state === 'connecting') {
      return
    }
    const Impl = this.WebSocketImpl ?? (globalThis as { WebSocket?: WebSocketConstructor }).WebSocket
    if (!Impl) {
      throw new Error(
        'No WebSocket implementation available. Inject one via the `WebSocket` option (e.g. the ' +
          '`ws` package on Node < 22).',
      )
    }
    this.closedByUser = false
    this.setState('connecting')
    const socket = new Impl(this.url, this.protocolsOrOptions)
    this.socket = socket

    socket.onopen = () => {
      this.reconnectAttempts = 0
      this.setState('open')
      // (Re)subscribe to every topic that currently has listeners — also covers reconnects.
      for (const topic of this.topicListeners.keys()) {
        this.send({ subscribe: topic })
      }
    }
    socket.onmessage = (event) => this.route(event.data)
    socket.onclose = () => this.handleDisconnect()
    socket.onerror = () => this.handleDisconnect()
  }

  /// Permanently closes the socket and cancels any pending reconnect. Registered listeners are
  /// kept, so a later `connect()` resumes them.
  close(): void {
    this.closedByUser = true
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
    const socket = this.socket
    this.socket = null
    if (socket) {
      socket.onopen = null
      socket.onmessage = null
      socket.onclose = null
      socket.onerror = null
      socket.close()
    }
    this.setState('closed')
  }

  /// Subscribes `listener` to a topic's monitoring batches. The first subscriber to a topic sends a
  /// `subscribe` frame; the last to leave sends `unsubscribe`. Connects lazily. Returns an
  /// unsubscribe function.
  subscribe(topic: string, listener: MonitoringListener): () => void {
    let listeners = this.topicListeners.get(topic)
    if (!listeners) {
      listeners = new Set()
      this.topicListeners.set(topic, listeners)
      this.send({ subscribe: topic })
    }
    listeners.add(listener)
    this.connect()

    return () => {
      const current = this.topicListeners.get(topic)
      if (!current) {
        return
      }
      current.delete(listener)
      if (current.size === 0) {
        this.topicListeners.delete(topic)
        this.send({ unsubscribe: topic })
      }
    }
  }

  /// Registers a listener for server notifications (e.g. `{ event: 'slaves_changed' }`). Connects
  /// lazily. Returns a removal function.
  onNotification(listener: NotificationListener): () => void {
    this.notificationListeners.add(listener)
    this.connect()
    return () => {
      this.notificationListeners.delete(listener)
    }
  }

  /// Registers a listener for procedure progress messages (reserved; fires once the backend ships
  /// them). Connects lazily. Returns a removal function.
  onProgress(listener: ProgressListener): () => void {
    this.progressListeners.add(listener)
    this.connect()
    return () => {
      this.progressListeners.delete(listener)
    }
  }

  /// Observes connection-state changes. Returns a removal function.
  onStateChange(listener: StateListener): () => void {
    this.stateListeners.add(listener)
    return () => {
      this.stateListeners.delete(listener)
    }
  }

  private setState(state: ReadyState): void {
    if (this.state === state) {
      return
    }
    this.state = state
    for (const listener of this.stateListeners) {
      listener(state)
    }
  }

  private send(payload: unknown): void {
    const socket = this.socket
    if (socket && socket.readyState === WS_OPEN) {
      socket.send(JSON.stringify(payload))
    }
  }

  private handleDisconnect(): void {
    if (this.socket) {
      this.socket.onopen = null
      this.socket.onmessage = null
      this.socket.onclose = null
      this.socket.onerror = null
      this.socket = null
    }
    this.setState('closed')
    if (this.closedByUser || this.reconnectDelayMs <= 0) {
      return
    }
    const delay = Math.min(
      this.reconnectDelayMs * 2 ** this.reconnectAttempts,
      this.maxReconnectDelayMs,
    )
    this.reconnectAttempts += 1
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null
      this.connect()
    }, delay)
  }

  private route(raw: unknown): void {
    if (typeof raw !== 'string') {
      return
    }
    let msg: unknown
    try {
      msg = JSON.parse(raw)
    } catch {
      return
    }
    if (!msg || typeof msg !== 'object') {
      return
    }
    const { type } = msg as { type?: unknown }
    if (type === 'monitoring') {
      const { topic, data } = msg as { topic?: unknown; data?: unknown }
      if (typeof topic === 'string' && Array.isArray(data)) {
        const listeners = this.topicListeners.get(topic)
        if (listeners) {
          for (const listener of listeners) {
            listener(data as MonitoringBatch)
          }
        }
      }
    } else if (type === 'notification') {
      const { data } = msg as { data?: unknown }
      for (const listener of this.notificationListeners) {
        listener(data)
      }
    } else if (type === 'progress') {
      const { data } = msg as { data?: unknown }
      for (const listener of this.progressListeners) {
        listener(data)
      }
    }
  }
}
