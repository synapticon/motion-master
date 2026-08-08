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
 *
 * That reach is why the probe is deliberately reluctant to declare the server
 * dead. It is one un-retried sample driving a switch that stops the whole UI
 * updating, and the moment that matters most is the one where it is most
 * tempted to fire: during a long procedure the server is busy, so a probe is
 * likelier to be slow or to lose a race — and a page reporting that
 * procedure's progress is exactly what must not freeze. Hence two guards:
 *
 * - **A generous timeout, not a tight one.** Without any timeout a request
 *   that never settles skips the `finally` and the probe never reschedules —
 *   permanently, and if it wedges while offline nothing is left to turn
 *   polling back on. But a short timeout is the opposite failure: a merely
 *   slow answer gets reported as a dead server. Several intervals' grace
 *   distinguishes "wedged" from "busy".
 * - **Two consecutive failures before going offline**, one success to come
 *   back. Offline stops everything, so it should need corroboration; online
 *   costs nothing to re-enter and should be instant.
 */
export function useApiHealth(api: Api, intervalMs = 2000): boolean {
  const [online, setOnline] = useState(false)

  useEffect(() => {
    let cancelled = false
    let timer: ReturnType<typeof setTimeout> | undefined
    let failures = 0

    async function check() {
      try {
        await api.getVersion({ signal: AbortSignal.timeout(intervalMs * 4) })
        if (!cancelled) {
          failures = 0
          setOnline(true)
          onlineManager.setOnline(true)
        }
      } catch {
        if (!cancelled && ++failures >= 2) {
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
