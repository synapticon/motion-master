import { createContext, useContext, useEffect, useRef, useSyncExternalStore } from 'react'

export interface RequestEntry {
  id: number
  method: string
  url: string
  requestBody?: string
  requestedAt: number
  completedAt?: number
  durationMs?: number
  status?: number
  statusText?: string
  contentType?: string
  responseBody?: string
  error?: string
}

const MAX_ENTRIES = 1000

class RequestsStore {
  private entries: RequestEntry[] = []
  private listeners = new Set<() => void>()
  private nextId = 1

  subscribe = (cb: () => void): (() => void) => {
    this.listeners.add(cb)
    return () => {
      this.listeners.delete(cb)
    }
  }

  getSnapshot = (): RequestEntry[] => this.entries

  private emit(): void {
    this.listeners.forEach(l => l())
  }

  start(method: string, url: string, requestBody?: string): number {
    const entry: RequestEntry = {
      id: this.nextId++,
      method,
      url,
      requestBody,
      requestedAt: Date.now(),
    }
    this.entries = [entry, ...this.entries].slice(0, MAX_ENTRIES)
    this.emit()
    return entry.id
  }

  complete(id: number, patch: Partial<RequestEntry>): void {
    this.entries = this.entries.map(e => (e.id === id ? { ...e, ...patch } : e))
    this.emit()
  }

  clear(): void {
    this.entries = []
    this.emit()
  }
}

const store = new RequestsStore()
const RequestsContext = createContext(store)

function shouldCapture(url: string): boolean {
  try {
    return new URL(url, window.location.origin).pathname.startsWith('/api')
  } catch {
    return false
  }
}

export function isHealthPollUrl(url: string): boolean {
  try {
    return new URL(url, window.location.origin).pathname === '/api/version'
  } catch {
    return false
  }
}

function bodyToString(body: BodyInit | null | undefined): string | undefined {
  if (body == null) {
    return undefined
  }
  if (typeof body === 'string') {
    return body
  }
  if (body instanceof URLSearchParams) {
    return body.toString()
  }
  if (body instanceof FormData) {
    const parts: string[] = []
    body.forEach((v, k) => {
      parts.push(`${k}=${typeof v === 'string' ? v : `[Blob ${v.size} bytes]`}`)
    })
    return parts.join('&')
  }
  if (body instanceof Blob) {
    return `[Blob ${body.size} bytes]`
  }
  if (body instanceof ArrayBuffer) {
    return `[ArrayBuffer ${body.byteLength} bytes]`
  }
  return undefined
}

export function RequestsProvider({ children }: { children: React.ReactNode }) {
  const installedRef = useRef(false)

  useEffect(() => {
    if (installedRef.current) {
      return
    }
    installedRef.current = true
    const original = window.fetch

    const wrapped: typeof fetch = async (input, init) => {
      const url =
        typeof input === 'string'
          ? input
          : input instanceof URL
            ? input.toString()
            : input.url
      if (!shouldCapture(url)) {
        return original(input, init)
      }

      const method = (
        init?.method ?? (input instanceof Request ? input.method : 'GET')
      ).toUpperCase()
      const requestBody = bodyToString(init?.body)
      const start = performance.now()
      const id = store.start(method, url, requestBody)

      try {
        const response = await original(input, init)
        const cloned = response.clone()
        const contentType = cloned.headers.get('content-type') ?? undefined
        let responseBody: string | undefined
        try {
          responseBody = await cloned.text()
        } catch {
          responseBody = undefined
        }
        store.complete(id, {
          completedAt: Date.now(),
          durationMs: performance.now() - start,
          status: response.status,
          statusText: response.statusText,
          contentType,
          responseBody,
        })
        return response
      } catch (err) {
        store.complete(id, {
          completedAt: Date.now(),
          durationMs: performance.now() - start,
          error: err instanceof Error ? err.message : String(err),
        })
        throw err
      }
    }

    window.fetch = wrapped
    return () => {
      window.fetch = original
      installedRef.current = false
    }
  }, [])

  return <RequestsContext.Provider value={store}>{children}</RequestsContext.Provider>
}

export function useRequests(): { entries: RequestEntry[]; clear: () => void } {
  const s = useContext(RequestsContext)
  const entries = useSyncExternalStore(s.subscribe, s.getSnapshot)
  return { entries, clear: () => s.clear() }
}
