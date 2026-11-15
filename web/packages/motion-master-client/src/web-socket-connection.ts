// Framework-agnostic connection to Motion Master's single WebSocket — the one bidirectional
// socket that carries server->client monitoring batches and notifications, and client->server
// topic subscribe/unsubscribe. It owns reconnection, multiplexes
// per-topic subscriptions over the one socket (ref-counted: subscribe on the first listener,
// unsubscribe on the last, re-subscribe everything on reconnect), and routes inbound frames by
// type. Nothing arrives until something is subscribed: `subscribe(topic, cb)` for one monitoring's
// batches, `onNotification(cb)` for every server notification — the latter is one shared topic, so
// a single subscription covers events added in a later server version. It is isomorphic: the
// browser global `WebSocket` is used by default, and any compatible implementation (e.g. the Node
// `ws` package) can be injected — there is no hard dependency on either, so the package stays
// usable in both environments.

/** Connection state, mirroring the three states the UI cares about. */
export type ReadyState = 'connecting' | 'open' | 'closed';

/**
 * One published monitoring batch for a topic: rows of `[timestampUs, v0, v1, ...]` — epoch
 * microseconds followed by one value per parameter (positionally ordered; fetch the order once
 * from `GET /api/monitorings/{topic}`). A value is `null` when its device was not exchanging at
 * sample time. The stream is lossless: one row per recorded cycle since the last flush.
 */
export type MonitoringRow = (number | null)[];
export type MonitoringBatch = MonitoringRow[];

export interface MonitoringMessage {
  type: 'monitoring';
  topic: string;
  data: MonitoringBatch;
}

/** The data of a notification. `event` names its kind, such as `short-working-counter`. */
export interface NotificationData {
  event: string;
  [field: string]: unknown;
}

export interface NotificationMessage {
  type: 'notification';
  data: NotificationData;
}

export type ServerMessage = MonitoringMessage | NotificationMessage;

type MonitoringListener = (rows: MonitoringBatch) => void;
type NotificationListener = (data: NotificationData) => void;
type StateListener = (state: ReadyState) => void;

/**
 * The numeric `readyState` value a WebSocket reports when it is open. Hard-coded (rather than read
 * off a global) so the client never depends on a particular `WebSocket` implementation's statics.
 */
const WS_OPEN = 1;

/**
 * The one topic every server notification is published to. One topic rather than one per event, so
 * a client subscribes once and keeps receiving events added in a later server version — uWebSockets
 * matches topic names exactly, with no wildcard, so there is no way to ask for "all of them".
 * Events are told apart by `data.event`, which is kebab-case.
 */
export const NOTIFICATION_TOPIC = 'notifications';

/**
 * Minimal structural shape of a WebSocket instance — satisfied by both the browser global and the
 * Node `ws` package (which implements the same `on*` property setters) — so neither is a hard
 * dependency.
 */
export interface WebSocketLike {
  readyState: number;
  send(data: string): void;
  close(): void;
  onopen: ((event: unknown) => void) | null;
  onclose: ((event: unknown) => void) | null;
  onerror: ((event: unknown) => void) | null;
  onmessage: ((event: { data: unknown }) => void) | null;
}

export type WebSocketConstructor = new (url: string, protocolsOrOptions?: unknown) => WebSocketLike;

export interface WebSocketConnectionOptions {
  /** Full WebSocket URL (e.g. `wss://host:62281`). */
  url: string;
  /** The WebSocket class. Default: the global `WebSocket` of the browser or of Node.js. */
  WebSocket?: WebSocketConstructor;
  /**
   * The second argument of the WebSocket constructor, such as `{ rejectUnauthorized: false }` for
   * the `ws` package. The global `WebSocket` takes no TLS option, and a browser refuses an object
   * here.
   */
  protocolsOrOptions?: unknown;
  /**
   * Base reconnect backoff in ms (doubled each consecutive failure up to `maxReconnectDelayMs`).
   * Set to 0 to disable auto-reconnect. Default 1000.
   */
  reconnectDelayMs?: number;
  /** Upper bound on the reconnect backoff in ms. Default 15000. */
  maxReconnectDelayMs?: number;
}

export class WebSocketConnection {
  private readonly url: string;
  private readonly WebSocketImpl?: WebSocketConstructor;
  private readonly protocolsOrOptions?: unknown;
  private readonly reconnectDelayMs: number;
  private readonly maxReconnectDelayMs: number;

  private socket: WebSocketLike | null = null;
  private state: ReadyState = 'closed';
  private closedByUser = false;
  // True once a caller asked for the connection with connect(). A connection that only a listener
  // opened closes when the last listener leaves, so that a Node.js program can end.
  private keepOpen = false;
  private reconnectAttempts = 0;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;

  private readonly topicListeners = new Map<string, Set<MonitoringListener>>();
  private readonly notificationListeners = new Set<NotificationListener>();
  private readonly stateListeners = new Set<StateListener>();

  constructor(options: WebSocketConnectionOptions) {
    this.url = options.url;
    this.WebSocketImpl = options.WebSocket;
    this.protocolsOrOptions = options.protocolsOrOptions;
    this.reconnectDelayMs = options.reconnectDelayMs ?? 1000;
    this.maxReconnectDelayMs = options.maxReconnectDelayMs ?? 15000;
  }

  get readyState(): ReadyState {
    return this.state;
  }

  /**
   * Opens the socket and keeps it open after the last listener leaves, until `close()`. Without this
   * call, a listener opens the socket, and the socket closes again when the last listener leaves.
   */
  connect(): void {
    this.keepOpen = true;
    this.open();
  }

  /**
   * Throws when no WebSocket class is available: none was passed, and the runtime has no global
   * `WebSocket`. A caller that is about to create something on the server checks this first.
   */
  assertAvailable(): void {
    this.implementation();
  }

  private implementation(): WebSocketConstructor {
    const Impl =
      this.WebSocketImpl ?? (globalThis as { WebSocket?: WebSocketConstructor }).WebSocket;
    if (!Impl) {
      throw new Error(
        'no WebSocket class: this runtime has no global WebSocket, so pass one with the ' +
          '`WebSocket` option, such as the class of the `ws` package',
      );
    }
    return Impl;
  }

  /** Opens the socket unless it is open or opening. */
  private open(): void {
    if (this.socket || this.state === 'connecting') {
      return;
    }
    const Impl = this.implementation();
    this.closedByUser = false;
    this.setState('connecting');
    let socket: WebSocketLike;
    try {
      socket = new Impl(this.url, this.protocolsOrOptions);
    } catch (err) {
      // A malformed URL (e.g. a bad host/port from stale saved settings) makes the WebSocket
      // constructor throw synchronously. Report it as a failed connection and stay closed rather
      // than letting it propagate and crash the caller; a bad URL won't fix itself, so there is no
      // point scheduling a reconnect. The consumer observes 'closed' and can correct the endpoint.
      this.setState('closed');
      console.error(`WebSocketConnection: failed to open ${this.url}`, err);
      return;
    }
    this.socket = socket;

    socket.onopen = () => {
      this.reconnectAttempts = 0;
      this.setState('open');
      // (Re)subscribe to every topic that currently has listeners — also covers reconnects.
      for (const topic of this.topicListeners.keys()) {
        this.send({ subscribe: topic });
      }
      if (this.notificationListeners.size > 0) {
        this.send({ subscribe: NOTIFICATION_TOPIC });
      }
    };
    socket.onmessage = (event) => this.route(event.data);
    socket.onclose = () => this.handleDisconnect();
    socket.onerror = () => this.handleDisconnect();
  }

  /**
   * Permanently closes the socket and cancels any pending reconnect. Registered listeners are
   * kept, so a later `connect()` resumes them.
   */
  close(): void {
    this.closedByUser = true;
    this.keepOpen = false;
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this.detach()?.close();
    this.setState('closed');
  }

  /**
   * Subscribes `listener` to a topic's monitoring batches. The first subscriber to a topic sends a
   * `subscribe` frame; the last to leave sends `unsubscribe`. Connects lazily. Returns an
   * unsubscribe function.
   */
  subscribe(topic: string, listener: MonitoringListener): () => void {
    // Throws before any state changes, so a listener is never left behind.
    this.implementation();
    let listeners = this.topicListeners.get(topic);
    if (!listeners) {
      listeners = new Set();
      this.topicListeners.set(topic, listeners);
      this.send({ subscribe: topic });
    }
    listeners.add(listener);
    this.open();

    return () => {
      const current = this.topicListeners.get(topic);
      if (!current) {
        return;
      }
      current.delete(listener);
      if (current.size === 0) {
        this.topicListeners.delete(topic);
        this.send({ unsubscribe: topic });
      }
      this.closeIfIdle();
    };
  }

  /**
   * Registers a listener for server notifications (e.g. `{ event: 'short-working-counter' }`).
   * Every notification arrives on one topic, so the first listener subscribes and the last to
   * leave unsubscribes — the same ref-counting `subscribe` uses. Connects lazily. Returns a
   * removal function.
   */
  onNotification(listener: NotificationListener): () => void {
    // Throws before any state changes, so a listener is never left behind.
    this.implementation();
    if (this.notificationListeners.size === 0) {
      this.send({ subscribe: NOTIFICATION_TOPIC });
    }
    this.notificationListeners.add(listener);
    this.open();
    return () => {
      this.notificationListeners.delete(listener);
      if (this.notificationListeners.size === 0) {
        this.send({ unsubscribe: NOTIFICATION_TOPIC });
      }
      this.closeIfIdle();
    };
  }

  /**
   * Calls `listener` each time the socket closes, with `true` when `close()` closed it. Returns a
   * removal function.
   */
  onClose(listener: (byUser: boolean) => void): () => void {
    return this.onStateChange((state) => {
      if (state === 'closed') {
        listener(this.closedByUser);
      }
    });
  }

  /** Observes connection-state changes. Returns a removal function. */
  onStateChange(listener: StateListener): () => void {
    this.stateListeners.add(listener);
    return () => {
      this.stateListeners.delete(listener);
    };
  }

  /**
   * Closes a socket that only listeners opened once none is left. A later listener opens it again.
   */
  private closeIfIdle(): void {
    const idle = this.topicListeners.size === 0 && this.notificationListeners.size === 0;
    if (this.keepOpen || !idle) {
      return;
    }
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this.detach()?.close();
    this.setState('closed');
  }

  /** Takes the socket from this connection and removes its handlers. Returns it, or `null`. */
  private detach(): WebSocketLike | null {
    const socket = this.socket;
    this.socket = null;
    if (socket) {
      socket.onopen = null;
      socket.onmessage = null;
      socket.onclose = null;
      socket.onerror = null;
    }
    return socket;
  }

  private setState(state: ReadyState): void {
    if (this.state === state) {
      return;
    }
    this.state = state;
    for (const listener of this.stateListeners) {
      listener(state);
    }
  }

  private send(payload: unknown): void {
    const socket = this.socket;
    if (socket && socket.readyState === WS_OPEN) {
      socket.send(JSON.stringify(payload));
    }
  }

  private handleDisconnect(): void {
    this.detach();
    this.setState('closed');
    if (this.closedByUser || this.reconnectDelayMs <= 0) {
      return;
    }
    const delay = Math.min(
      this.reconnectDelayMs * 2 ** this.reconnectAttempts,
      this.maxReconnectDelayMs,
    );
    this.reconnectAttempts += 1;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.open();
    }, delay);
  }

  private route(raw: unknown): void {
    if (typeof raw !== 'string') {
      return;
    }
    let msg: unknown;
    try {
      msg = JSON.parse(raw);
    } catch {
      return;
    }
    if (!msg || typeof msg !== 'object') {
      return;
    }
    const { type } = msg as { type?: unknown };
    if (type === 'monitoring') {
      const { topic, data } = msg as { topic?: unknown; data?: unknown };
      if (typeof topic === 'string' && Array.isArray(data)) {
        const listeners = this.topicListeners.get(topic);
        if (listeners) {
          for (const listener of listeners) {
            listener(data as MonitoringBatch);
          }
        }
      }
    } else if (type === 'notification') {
      const { data } = msg as { data?: NotificationData };
      if (typeof data !== 'object' || data === null) {
        return;
      }
      for (const listener of this.notificationListeners) {
        listener(data);
      }
    }
  }
}
