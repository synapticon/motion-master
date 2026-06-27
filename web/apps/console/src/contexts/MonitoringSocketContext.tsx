import { createContext, useCallback, useContext, useEffect, useRef, useState } from 'react'
import type { ReactNode } from 'react'
import { WebSocketConnection, type MonitoringBatch, type ReadyState } from '@synapticon/motion-master-client'

// One published batch for a topic: an array of sample rows, each [timestampUs, v0, v1, ...].
// A value is null when its device was not exchanging at sample time.
export type SampleRows = MonitoringBatch
type BatchListener = (rows: SampleRows) => void

interface MonitoringSocketValue {
  readyState: ReadyState
  /// Subscribes @p listener to a topic's batches. The first subscriber to a topic sends a
  /// `subscribe` frame; the last to unsubscribe sends `unsubscribe`. Returns an unsubscribe fn.
  subscribe: (topic: string, listener: BatchListener) => () => void
}

const MonitoringSocketContext = createContext<MonitoringSocketValue | null>(null)

/// Thin React wrapper over the SDK's framework-agnostic `WebSocketConnection`. Reconnection,
/// per-topic subscribe/unsubscribe multiplexing, and frame routing all live in the connection; this
/// provider just owns one instance for its subtree and surfaces `subscribe` + `readyState` to the
/// monitoring cards. `url` is stable for the provider's lifetime (the endpoint is edited on a
/// different page), so a live endpoint switch is handled by remounting the provider with `key={url}`.
export function MonitoringSocketProvider({ url, children }: { url: string; children: ReactNode }) {
  const connectionRef = useRef<WebSocketConnection | null>(null)
  if (connectionRef.current === null) {
    connectionRef.current = new WebSocketConnection({ url })
  }
  const client = connectionRef.current

  const [readyState, setReadyState] = useState<ReadyState>(client.readyState)

  useEffect(() => {
    const off = client.onStateChange(setReadyState)
    client.connect()
    setReadyState(client.readyState)
    return () => {
      off()
      client.close()
    }
  }, [client])

  const subscribe = useCallback(
    (topic: string, listener: BatchListener) => client.subscribe(topic, listener),
    [client],
  )

  return (
    <MonitoringSocketContext.Provider value={{ readyState, subscribe }}>
      {children}
    </MonitoringSocketContext.Provider>
  )
}

export function useMonitoringSocket(): MonitoringSocketValue {
  const ctx = useContext(MonitoringSocketContext)
  if (!ctx) {
    throw new Error('useMonitoringSocket must be used within a MonitoringSocketProvider')
  }
  return ctx
}
