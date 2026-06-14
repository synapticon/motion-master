import { createContext, useCallback, useContext, useMemo, useState } from 'react'
import { Api } from '@synapticon/motion-master-client'

const SESSION_KEY = 'mm:session'
const ENDPOINT_KEY = 'mm:endpoint'

const DEFAULT_HOST = 'local.motion-master.synapticon.com'
const DEFAULT_HTTP_PORT = '61447'
const DEFAULT_WS_PORT = '62281'

export interface Session {
  driver: 'soem' | 'spoe' | 'igh'
  adapter: string
}

export function readSession(): Session | null {
  try {
    const raw = sessionStorage.getItem(SESSION_KEY)
    return raw ? (JSON.parse(raw) as Session) : null
  } catch {
    return null
  }
}

interface Endpoint {
  host: string
  httpPort: string
  wsPort: string
}

// The endpoint config persists to localStorage so a configured host/port survives reloads.
function readEndpoint(): Endpoint {
  const fallback: Endpoint = { host: DEFAULT_HOST, httpPort: DEFAULT_HTTP_PORT, wsPort: DEFAULT_WS_PORT }
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
  driver: 'soem' | 'spoe' | 'igh'
  setDriver: (d: 'soem' | 'spoe' | 'igh') => void
  adapter: string
  setAdapter: (a: string) => void
  hasScanned: boolean
  setHasScanned: (val: boolean) => void
  /// True once the fieldbus driver is initialized on the server (after a
  /// successful init, a 409 "already initialized", or a restored session).
  /// Cleared on reset.
  isInitialized: boolean
  setIsInitialized: (val: boolean) => void
  /// True when init() was skipped because the fieldbus was already initialized
  /// on the server (e.g. after a browser refresh that reused the stored session).
  alreadyInitialized: boolean
  setAlreadyInitialized: (val: boolean) => void
}

const ConnectionContext = createContext<ConnectionContextValue | null>(null)

export function ConnectionProvider({ children }: { children: React.ReactNode }) {
  const stored = readSession()
  const endpoint = readEndpoint()
  const [host, setHostState] = useState(endpoint.host)
  const [httpPort, setHttpPortState] = useState(endpoint.httpPort)
  const [wsPort, setWsPortState] = useState(endpoint.wsPort)
  const [driver, setDriver] = useState<'soem' | 'spoe' | 'igh'>(stored?.driver ?? 'soem')
  const [adapter, setAdapter] = useState(stored?.adapter ?? '')
  const [hasScanned, setHasScannedState] = useState(false)
  const [isInitialized, setIsInitialized] = useState(false)
  const [alreadyInitialized, setAlreadyInitialized] = useState(false)

  // Persist the endpoint config on every change so a configured host/port survives reloads.
  const setHost = useCallback((value: string) => {
    setHostState(value)
    writeEndpoint({ host: value, httpPort, wsPort })
  }, [httpPort, wsPort])
  const setHttpPort = useCallback((value: string) => {
    setHttpPortState(value)
    writeEndpoint({ host, httpPort: value, wsPort })
  }, [host, wsPort])
  const setWsPort = useCallback((value: string) => {
    setWsPortState(value)
    writeEndpoint({ host, httpPort, wsPort: value })
  }, [host, httpPort])

  const resetEndpoint = useCallback(() => {
    setHostState(DEFAULT_HOST)
    setHttpPortState(DEFAULT_HTTP_PORT)
    setWsPortState(DEFAULT_WS_PORT)
    writeEndpoint({ host: DEFAULT_HOST, httpPort: DEFAULT_HTTP_PORT, wsPort: DEFAULT_WS_PORT })
  }, [])

  const api = useMemo(
    () => new Api({ baseUrl: `https://${host}:${httpPort}` }),
    [host, httpPort],
  )

  const setHasScanned = useCallback(
    (val: boolean) => {
      setHasScannedState(val)
      if (val) {
        sessionStorage.setItem(SESSION_KEY, JSON.stringify({ driver, adapter }))
      } else {
        sessionStorage.removeItem(SESSION_KEY)
      }
    },
    [driver, adapter],
  )

  return (
    <ConnectionContext.Provider
      value={{ host, httpPort, wsPort, setHost, setHttpPort, setWsPort, resetEndpoint, api, driver, setDriver, adapter, setAdapter, hasScanned, setHasScanned, isInitialized, setIsInitialized, alreadyInitialized, setAlreadyInitialized }}
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
