import { createContext, useCallback, useContext, useEffect, useMemo, useState } from 'react'
import { Api } from '@synapticon/motion-master-client'
import { useApiHealth } from '../hooks/useApiHealth'

const ENDPOINT_KEY = 'mm:endpoint'

const DEFAULT_HOST = 'local.motion-master.synapticon.com'
const DEFAULT_HTTP_PORT = '61447'
const DEFAULT_WS_PORT = '62281'

type Driver = 'soem' | 'spoe'

interface Endpoint {
  host: string
  httpPort: string
  wsPort: string
  // The last-used connection target. Persisted purely as a convenience default for the
  // Init form — connection *state* (initialized / scanned) is derived from the server on
  // open (see ServerStateProbe), never from storage.
  driver: Driver
  adapter: string
}

// The endpoint config persists to localStorage so a configured host/port survives reloads.
function readEndpoint(): Endpoint {
  const fallback: Endpoint = {
    host: DEFAULT_HOST,
    httpPort: DEFAULT_HTTP_PORT,
    wsPort: DEFAULT_WS_PORT,
    driver: 'soem',
    adapter: '',
  }
  try {
    const raw = localStorage.getItem(ENDPOINT_KEY)
    return raw ? { ...fallback, ...(JSON.parse(raw) as Partial<Endpoint>) } : fallback
  } catch {
    return fallback
  }
}

function writeEndpoint(endpoint: Endpoint): void {
  try {
    localStorage.setItem(ENDPOINT_KEY, JSON.stringify(endpoint))
  } catch {
    // Ignore storage failures (e.g. private mode) — the in-memory value still applies.
  }
}

interface ConnectionContextValue {
  host: string
  httpPort: string
  /// WebSocket port — separate from the HTTP API port, since the backend runs the
  /// monitoring/control socket on its own port and loop.
  wsPort: string
  setHost: (host: string) => void
  setHttpPort: (httpPort: string) => void
  setWsPort: (wsPort: string) => void
  /// Reset host/httpPort/wsPort back to the built-in defaults (and persist them).
  resetEndpoint: () => void
  api: Api
  /// True while the Motion Master HTTP API is reachable at the current endpoint.
  /// Driven by a single `useApiHealth` probe here (the sole writer, so it also
  /// stays the single writer of React Query's onlineManager) and consumed by
  /// RootLayout (sidebar dot / link gating) and ConnectionPage (offline state).
  online: boolean
  driver: Driver
  setDriver: (d: Driver) => void
  adapter: string
  setAdapter: (a: string) => void
  hasScanned: boolean
  setHasScanned: (val: boolean) => void
  /// True once the fieldbus driver is initialized on the server (after a
  /// successful init, a 409 "already initialized", or when the on-open probe
  /// found devices already present). Cleared on reset.
  isInitialized: boolean
  setIsInitialized: (val: boolean) => void
  /// True when init() was skipped because the fieldbus was already initialized
  /// on the server — either a manual init returned 409, or the on-open probe
  /// (ServerStateProbe) found an already-scanned bus from another instance.
  alreadyInitialized: boolean
  setAlreadyInitialized: (val: boolean) => void
}

const ConnectionContext = createContext<ConnectionContextValue | null>(null)

export function ConnectionProvider({ children }: { children: React.ReactNode }) {
  const endpoint = readEndpoint()
  const [host, setHost] = useState(endpoint.host)
  const [httpPort, setHttpPort] = useState(endpoint.httpPort)
  const [wsPort, setWsPort] = useState(endpoint.wsPort)
  const [driver, setDriver] = useState<Driver>(endpoint.driver)
  const [adapter, setAdapter] = useState(endpoint.adapter)
  const [hasScanned, setHasScanned] = useState(false)
  const [isInitialized, setIsInitialized] = useState(false)
  const [alreadyInitialized, setAlreadyInitialized] = useState(false)

  // Persist the endpoint config (host/port + last-used connection target) on every change
  // so it survives reloads. This is a convenience default only; connection *state* is
  // derived from the server on open (see ServerStateProbe), never from storage.
  useEffect(() => {
    writeEndpoint({ host, httpPort, wsPort, driver, adapter })
  }, [host, httpPort, wsPort, driver, adapter])

  const resetEndpoint = useCallback(() => {
    setHost(DEFAULT_HOST)
    setHttpPort(DEFAULT_HTTP_PORT)
    setWsPort(DEFAULT_WS_PORT)
  }, [])

  const api = useMemo(
    () => new Api({ baseUrl: `https://${host}:${httpPort}` }),
    [host, httpPort],
  )

  const online = useApiHealth(api)

  return (
    <ConnectionContext.Provider
      value={{ host, httpPort, wsPort, setHost, setHttpPort, setWsPort, resetEndpoint, api, online, driver, setDriver, adapter, setAdapter, hasScanned, setHasScanned, isInitialized, setIsInitialized, alreadyInitialized, setAlreadyInitialized }}
    >
      {children}
    </ConnectionContext.Provider>
  )
}

export function useConnection() {
  const ctx = useContext(ConnectionContext)
  if (!ctx) throw new Error('useConnection must be used within ConnectionProvider')
  return ctx
}
