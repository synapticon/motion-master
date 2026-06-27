import { useEffect, useRef, useState } from 'react'

export type ReadyState = 'connecting' | 'open' | 'closed'

export function useWebSocket(url: string) {
  const [lastMessage, setLastMessage] = useState<MessageEvent | null>(null)
  const [readyState, setReadyState] = useState<ReadyState>('connecting')
  const wsRef = useRef<WebSocket | null>(null)

  useEffect(() => {
    const ws = new WebSocket(url)
    wsRef.current = ws

    ws.onopen = () => setReadyState('open')
    ws.onclose = () => setReadyState('closed')
    ws.onerror = () => setReadyState('closed')
    ws.onmessage = (event) => setLastMessage(event)

    return () => ws.close()
  }, [url])

  return { lastMessage, readyState }
}
