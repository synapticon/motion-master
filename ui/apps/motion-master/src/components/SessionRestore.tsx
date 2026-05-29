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

    const restore = () =>
      api.scan().then(() => {
        setHasScanned(true)
        queryClient.invalidateQueries({ queryKey: ['devices'] })
      })

    api.init({ driver: session.driver, adapter: session.adapter })
      .then(restore)
      .catch((err: unknown) => {
        // 409 = the server is still initialized from before the refresh. The
        // fieldbus is live, so just re-scan and restore the session rather than
        // dropping it; flag it so the Dashboard can explain why init was skipped.
        if (err && typeof err === 'object' && (err as { status?: number }).status === 409) {
          setAlreadyInitialized(true)
          restore().catch(() => sessionStorage.removeItem('mm:session'))
          return
        }
        sessionStorage.removeItem('mm:session')
      })
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  return <>{children}</>
}
