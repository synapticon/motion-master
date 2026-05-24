import { useEffect, useRef } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { readSession } from '../contexts/ConnectionContext'
import { useConnection } from '../contexts/ConnectionContext'

export function SessionRestore({ children }: { children: React.ReactNode }) {
  const { api, setHasScanned } = useConnection()
  const queryClient = useQueryClient()
  const attempted = useRef(false)

  useEffect(() => {
    if (attempted.current) return
    attempted.current = true

    const session = readSession()
    if (!session) return

    api.init({ driver: session.driver, adapter: session.adapter })
      .then(() => api.scan())
      .then(() => {
        setHasScanned(true)
        queryClient.invalidateQueries({ queryKey: ['devices'] })
      })
      .catch(() => {
        sessionStorage.removeItem('mm:session')
      })
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  return <>{children}</>
}
