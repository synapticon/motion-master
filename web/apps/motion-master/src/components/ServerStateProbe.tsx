import { useEffect } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { useConnection } from '../contexts/ConnectionContext'

// On open, derive connection state from the server rather than from any client-local
// storage: the server (DeviceManager) is the source of truth and remembers the
// initialized driver, the scanned device list, and per-device AL states for its whole
// lifetime. A freshly-opened instance (new tab/window, or after a browser restart) probes
// GET /api/devices once; a non-empty list means another instance already initialized and
// scanned the bus, so we light up the view without re-running init/scan. Re-probes whenever
// `api` changes (the user repointed host/port on the Connection page).
export function ServerStateProbe({ children }: { children: React.ReactNode }) {
  const { api, setHasScanned, setIsInitialized, setAlreadyInitialized } = useConnection()
  const queryClient = useQueryClient()

  useEffect(() => {
    let cancelled = false

    api
      .getDevices()
      .then((res) => {
        // Empty list = server not initialized, or an initialized-but-empty bus. Either
        // way keep the "Not initialized" view; do NOT init/scan from here.
        if (cancelled || res.data.length === 0) return
        // Devices already present → the bus is live and scanned. Seed the cache so the
        // gated ['devices'] queries have data immediately, then pull current AL states.
        queryClient.setQueryData(['devices'], res)
        setIsInitialized(true)
        setHasScanned(true)
        setAlreadyInitialized(true)
        void queryClient.invalidateQueries({ queryKey: ['deviceStates'] })
      })
      // Server down / unreachable / uninitialized — nothing to restore, stay on the
      // Connection view.
      .catch(() => {})

    return () => {
      cancelled = true
    }
  }, [api]) // eslint-disable-line react-hooks/exhaustive-deps

  return <>{children}</>
}
