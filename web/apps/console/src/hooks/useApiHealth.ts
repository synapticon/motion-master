import { useEffect, useState } from 'react'
import type { Api } from '@synapticon/motion-master-client'

/**
 * Polls `GET /api/version` to determine whether the Motion Master API is live.
 * Returns `true` once a request succeeds, `false` while the server is
 * unreachable. Re-checks every `intervalMs` (default 2s).
 *
 * The probe hits `local.motion-master.synapticon.com` (→ 127.0.0.1), so each
 * request is ~1ms and effectively free; the interval is a UI-responsiveness
 * knob (how fast the online/offline dot and sidebar react), not a load concern.
 */
export function useApiHealth(api: Api, intervalMs = 2000): boolean {
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
