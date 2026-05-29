import { useEffect, useRef } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { readSession } from '../contexts/ConnectionContext'
import { useConnection } from '../contexts/ConnectionContext'

export function SessionRestore({ children }: { children: React.ReactNode }) {
  const { api, setHasScanned, setAlreadyInitialized } = useConnection()
  const queryClient = useQueryClient()
  const attempted = useRef(false)

  useEffect(() => {
    if (attempted.current) return
    attempted.current = true

    const session = readSession()
    if (!session) return

    api.init({ driver: session.driver, adapter: session.adapter })
      // Fresh init succeeded — this is a first connection, so discover the bus.
      .then(() =>
        api.scan().then(() => {
          setHasScanned(true)
          queryClient.invalidateQueries({ queryKey: ['devices'] })
        }),
      )
      .catch((err: unknown) => {
        // 409 = the server is still initialized from before the refresh. The
        // fieldbus is live and already scanned, so just re-fetch the existing
        // device list to restore the view. Do NOT re-scan: ecx_config_init
        // re-discovers the bus and would reset every slave back to INIT, losing
        // the state the user already brought devices to (e.g. PRE-OP).
        if (err && typeof err === 'object' && (err as { status?: number }).status === 409) {
          setAlreadyInitialized(true)
          setHasScanned(true)
          queryClient.invalidateQueries({ queryKey: ['devices'] })
          return
        }
        sessionStorage.removeItem('mm:session')
      })
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  return <>{children}</>
}
