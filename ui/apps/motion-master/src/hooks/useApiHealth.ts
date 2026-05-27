import { useEffect, useState } from 'react'
import type { Api } from '@mm/api-client'

/**
 * Polls `GET /api/version` to determine whether the Motion Master API is live.
 * Returns `true` once a request succeeds, `false` while the server is
 * unreachable. Re-checks every `intervalMs` (default 5s).
 */
export function useApiHealth(api: Api, intervalMs = 5000): boolean {
  const [online, setOnline] = useState(false)

  useEffect(() => {
    let cancelled = false
    let timer: ReturnType<typeof setTimeout> | undefined

    async function check() {
      try {
        await api.getVersion()
        if (!cancelled) {
          setOnline(true)
        }
      } catch {
        if (!cancelled) {
          setOnline(false)
        }
      } finally {
        if (!cancelled) {
          timer = setTimeout(check, intervalMs)
        }
      }
    }

    check()
    return () => {
      cancelled = true
      clearTimeout(timer)
    }
  }, [api, intervalMs])

  return online
}
