import { createContext, useContext, useMemo, useState } from 'react'
import { Api } from '@mm/api-client'

interface ConnectionContextValue {
  host: string
  port: string
  setHost: (host: string) => void
  setPort: (port: string) => void
  api: Api
  hasScanned: boolean
  setHasScanned: (val: boolean) => void
}

const ConnectionContext = createContext<ConnectionContextValue | null>(null)

export function ConnectionProvider({ children }: { children: React.ReactNode }) {
  const [host, setHost] = useState('local.motion-master.synapticon.com')
  const [port, setPort] = useState('8443')
  const [hasScanned, setHasScanned] = useState(false)

  const api = useMemo(
    () => new Api({ baseUrl: `https://${host}:${port}` }),
    [host, port],
  )

  return (
    <ConnectionContext.Provider value={{ host, port, setHost, setPort, api, hasScanned, setHasScanned }}>
      {children}
    </ConnectionContext.Provider>
  )
}

export function useConnection() {
  const ctx = useContext(ConnectionContext)
  if (!ctx) throw new Error('useConnection must be used within ConnectionProvider')
  return ctx
}
