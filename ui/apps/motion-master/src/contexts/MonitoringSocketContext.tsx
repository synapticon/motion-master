import { createContext, useCallback, useContext, useEffect, useRef, useState } from 'react'
import type { ReactNode } from 'react'
import type { ReadyState } from '../hooks/useWebSocket'

// One published batch for a topic: an array of sample rows, each [timestampMs, v0, v1, ...].
// A value is null when its device was not exchanging at sample time.
export type SampleRows = (number | null)[][]
type BatchListener = (rows: SampleRows) => void

interface MonitoringSocketValue {
  readyState: ReadyState
  /// Subscribes @p listener to a topic's batches. The first subscriber to a topic sends a
  /// `subscribe` frame; the last to unsubscribe sends `unsubscribe`. Returns an unsubscribe fn.
  subscribe: (topic: string, listener: BatchListener) => () => void
}

const MonitoringSocketContext = createContext<MonitoringSocketValue | null>(null)

/// Owns a single monitoring WebSocket and fans its batches out to per-topic listeners, so every
/// monitoring card on the page shares one connection (matching the server's topic pub/sub).
export function MonitoringSocketProvider({ url, children }: { url: string; children: ReactNode }) {
  const [readyState, setReadyState] = useState<ReadyState>('connecting')
  const wsRef = useRef<WebSocket | null>(null)
  const listenersRef = useRef<Map<string, Set<BatchListener>>>(new Map())

  useEffect(() => {
    const ws = new WebSocket(url)
    wsRef.current = ws

    ws.onopen = () => {
      setReadyState('open')
      // (Re)subscribe to every topic that currently has listeners — covers reconnects too.
      for (const topic of listenersRef.current.keys()) {
        ws.send(JSON.stringify({ subscribe: topic }))
      }
    }
    ws.onclose = () => setReadyState('closed')
    ws.onerror = () => setReadyState('closed')
    ws.onmessage = (event) => {
      let msg: unknown
      try {
        msg = JSON.parse(event.data as string)
      } catch {
        return
      }
      if (
        msg &&
        typeof msg === 'object' &&
        (msg as { type?: unknown }).type === 'monitoring' &&
        typeof (msg as { topic?: unknown }).topic === 'string' &&
        Array.isArray((msg as { data?: unknown }).data)
      ) {
        const { topic, data } = msg as { topic: string; data: SampleRows }
        const listeners = listenersRef.current.get(topic)
        if (listeners) {
          for (const listener of listeners) listener(data)
        }
      }
    }

    return () => {
      wsRef.current = null
      ws.close()
    }
  }, [url])

  const subscribe = useCallback((topic: string, listener: BatchListener) => {
    let set = listenersRef.current.get(topic)
    if (!set) {
      set = new Set()
      listenersRef.current.set(topic, set)
      const ws = wsRef.current
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ subscribe: topic }))
      }
    }
    set.add(listener)

    return () => {
      const current = listenersRef.current.get(topic)
      if (!current) return
      current.delete(listener)
      if (current.size === 0) {
        listenersRef.current.delete(topic)
        const ws = wsRef.current
        if (ws && ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ unsubscribe: topic }))
        }
      }
    }
  }, [])

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
