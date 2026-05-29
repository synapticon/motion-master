import { createContext, useCallback, useContext, useMemo, useState } from 'react'
import { Api } from '@mm/api-client'

const SESSION_KEY = 'mm:session'

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

interface ConnectionContextValue {
  host: string
  port: string
  setHost: (host: string) => void
  setPort: (port: string) => void
  api: Api
  driver: 'soem' | 'spoe' | 'igh'
  setDriver: (d: 'soem' | 'spoe' | 'igh') => void
  adapter: string
  setAdapter: (a: string) => void
  hasScanned: boolean
  setHasScanned: (val: boolean) => void
  /// True when init() was skipped because the fieldbus was already initialized
  /// on the server (e.g. after a browser refresh that reused the stored session).
  alreadyInitialized: boolean
  setAlreadyInitialized: (val: boolean) => void
}

const ConnectionContext = createContext<ConnectionContextValue | null>(null)

export function ConnectionProvider({ children }: { children: React.ReactNode }) {
  const stored = readSession()
  const [host, setHost] = useState('local.motion-master.synapticon.com')
  const [port, setPort] = useState('8443')
  const [driver, setDriver] = useState<'soem' | 'spoe' | 'igh'>(stored?.driver ?? 'soem')
  const [adapter, setAdapter] = useState(stored?.adapter ?? '')
  const [hasScanned, setHasScannedState] = useState(false)
  const [alreadyInitialized, setAlreadyInitialized] = useState(false)

  const api = useMemo(
    () => new Api({ baseUrl: `https://${host}:${port}` }),
    [host, port],
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
      value={{ host, port, setHost, setPort, api, driver, setDriver, adapter, setAdapter, hasScanned, setHasScanned, alreadyInitialized, setAlreadyInitialized }}
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
