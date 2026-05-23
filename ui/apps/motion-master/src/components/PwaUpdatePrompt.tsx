import { useRegisterSW } from 'virtual:pwa-register/react'

export default function PwaUpdatePrompt() {
  const {
    offlineReady: [offlineReady, setOfflineReady],
    needRefresh: [needRefresh, setNeedRefresh],
    updateServiceWorker,
  } = useRegisterSW({
    onRegistered(r) {
      r && setInterval(() => { r.update() }, 60 * 60 * 1000)
    },
  })

  const dismiss = () => {
    setOfflineReady(false)
    setNeedRefresh(false)
  }

  if (!offlineReady && !needRefresh) return null

  return (
    <div className="fixed bottom-4 right-4 z-50 flex items-center gap-3 bg-ocean-dark border border-white/20 px-4 py-3 shadow-lg text-white text-sm font-body">
      {needRefresh ? (
        <>
          <span>New version available.</span>
          <button
            onClick={() => updateServiceWorker(true)}
            className="text-syn-red hover:text-white transition-colors font-medium"
          >
            Update
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
