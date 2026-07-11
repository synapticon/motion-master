import { useEffect, useState } from 'react'
import { onlineManager } from '@tanstack/react-query'
import type { Api } from '@synapticon/motion-master-client'

/**
 * Polls `GET /api/version` to determine whether the Motion Master API is live.
 * Returns `true` once a request succeeds, `false` while the server is
 * unreachable. Re-checks every `intervalMs` (default 2s).
 *
 * The probe hits `local.motion-master.synapticon.com` (→ 127.0.0.1), so each
 * request is ~1ms and effectively free; the interval is a UI-responsiveness
 * knob (how fast the online/offline dot and sidebar react), not a load concern.
 *
 * The result is also mirrored into React Query's {@link onlineManager}. With
 * the default `networkMode: 'online'`, that pauses *every* query and mutation
 * (including `refetchInterval` polling on any page) the moment the API drops,
 * and resumes them on reconnect — so no query needs to gate on `online` by
 * hand. The probe itself uses the raw `api` client (not React Query), so it
 * keeps running while everything else is paused and can flip the switch back.
 * Only one caller should drive this (RootLayout), so there is a single writer.
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
          onlineManager.setOnline(true)
        }
      } catch {
        if (!cancelled) {
          setOnline(false)
          onlineManager.setOnline(false)
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
