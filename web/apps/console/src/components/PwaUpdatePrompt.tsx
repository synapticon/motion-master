import { useState } from 'react'
import { useRegisterSW } from 'virtual:pwa-register/react'

// If the new worker never takes control, the page reloads anyway after this time.
const FALLBACK_RELOAD_MS = 3000

// Activates the waiting service worker and reloads once it controls the page. The plugin's
// updateServiceWorker reloads only on workbox-window's 'controlling' event with isUpdate set, which
// is true only when this tab's own Workbox instance installed the worker. A worker installed in an
// earlier visit or in another tab activates with no reload, so the old app stays on screen. The
// browser's 'controllerchange' event fires in every case.
async function activateUpdate(): Promise<void> {
  const registration = await navigator.serviceWorker?.getRegistration()
  const waiting = registration?.waiting
  if (!waiting) {
    window.location.reload()
    return
  }
  navigator.serviceWorker.addEventListener('controllerchange', () => window.location.reload(), { once: true })
  waiting.postMessage({ type: 'SKIP_WAITING' })
  setTimeout(() => window.location.reload(), FALLBACK_RELOAD_MS)
}

export default function PwaUpdatePrompt() {
  const {
    offlineReady: [offlineReady, setOfflineReady],
    needRefresh: [needRefresh, setNeedRefresh],
  } = useRegisterSW({
    onRegistered(r) {
      r && setInterval(() => { r.update() }, 60 * 60 * 1000)
    },
  })
  const [updating, setUpdating] = useState(false)

  const dismiss = () => {
    setOfflineReady(false)
    setNeedRefresh(false)
  }

  if (!offlineReady && !needRefresh) return null

  return (
    <div className="fixed bottom-4 right-4 z-50 flex items-center gap-3 bg-ocean-dark border border-white/20 px-4 py-3 shadow-overlay-2 text-white text-sm font-body">
      {needRefresh ? (
        <>
          <span>New version available.</span>
          <button
            disabled={updating}
            onClick={() => {
              setUpdating(true)
              void activateUpdate()
            }}
            className="text-syn-red hover:text-white disabled:text-white/40 transition-colors font-medium"
          >
            {updating ? 'Updating…' : 'Update'}
          </button>
        </>
      ) : (
        <span>Ready to work offline.</span>
      )}
      <button onClick={dismiss} className="text-white/40 hover:text-white transition-colors ml-1">
        ×
      </button>
    </div>
  )
}
