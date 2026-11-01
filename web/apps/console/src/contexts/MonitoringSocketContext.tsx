import { createContext, useCallback, useContext, useEffect, useState } from 'react'
import type { ReactNode } from 'react'
import type { MonitoringBatch, ReadyState } from '@synapticon/motion-master-client'
import { useConnection } from './ConnectionContext'

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

/// Surfaces the WebSocket of the connection's `MotionMasterClient` to the monitoring cards as
/// `subscribe` and `readyState`. Reconnection, per-topic subscribe/unsubscribe multiplexing, and
/// frame routing all live in the client's `WebSocketConnection`. `ConnectionProvider` owns the
/// client and closes it, so this provider opens the socket and never closes it.
function MonitoringSocketProvider({ children }: { children: ReactNode }) {
  const { ws } = useConnection().client

  const [readyState, setReadyState] = useState<ReadyState>(ws.readyState)

  useEffect(() => {
    const off = ws.onStateChange(setReadyState)
    // Opens the socket again after StrictMode's simulated unmount, in which ConnectionProvider
    // closes the client.
    ws.connect()
    setReadyState(ws.readyState)
    return off
  }, [ws])

  const subscribe = useCallback(
    (topic: string, listener: BatchListener) => ws.subscribe(topic, listener),
    [ws],
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

/// App-level provider. Mount it once at the root (inside `ConnectionProvider`); every page then
/// shares one socket via `useMonitoringSocket()`. The connection multiplexes topics and ref-counts
/// subscribe/unsubscribe, so subscriptions survive navigation with no reconnect churn — a live view
/// starts streaming the moment it subscribes instead of waiting for a fresh socket to open. Keyed on
/// the WebSocket URL, so changing it on the Connection page remounts the subtree against the new
/// client.
export function AppMonitoringSocketProvider({ children }: { children: ReactNode }) {
  const { host, wsPort } = useConnection()
  return <MonitoringSocketProvider key={`wss://${host}:${wsPort}`}>{children}</MonitoringSocketProvider>
}
