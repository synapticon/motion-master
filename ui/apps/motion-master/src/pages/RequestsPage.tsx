import { useMemo, useState } from 'react'
import PageHeader from '../components/PageHeader'
import { isHealthPollUrl, useRequests, type RequestEntry } from '../contexts/RequestsContext'

const btnOutlineCls =
  'border border-syn-red text-syn-red px-3 py-1.5 text-xs hover:bg-syn-red hover:text-white disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

function StatusBadge({ status, error }: { status?: number; error?: string }) {
  let color = 'bg-grey-100 text-grey-500'
  let label: string = '…'
  if (error) {
    color = 'bg-status-bad/15 text-status-bad'
    label = 'ERR'
  } else if (status != null) {
    label = String(status)
    if (status >= 200 && status < 300) {
      color = 'bg-status-good/15 text-status-good'
    } else if (status >= 400) {
      color = 'bg-status-bad/15 text-status-bad'
    } else {
      color = 'bg-status-warn/15 text-status-warn'
    }
  }
  return (
    <span className={`inline-block min-w-12 text-center px-2 py-0.5 text-xs font-mono ${color}`}>
      {label}
    </span>
  )
}

function formatTime(ts?: number): string {
  if (!ts) {
    return '—'
  }
  const d = new Date(ts)
  return (
    d.toLocaleTimeString('en-GB', { hour12: false }) +
    '.' +
    String(d.getMilliseconds()).padStart(3, '0')
  )
}

function pathOf(rawUrl: string): string {
  try {
    const u = new URL(rawUrl, window.location.origin)
    return u.pathname + u.search
  } catch {
    return rawUrl
  }
}

// Wrap a value in single quotes, escaping embedded single quotes the POSIX way ('\'').
function shSingleQuote(s: string): string {
  return `'${s.replace(/'/g, "'\\''")}'`
}

// Headers that fetch/the browser sets itself — pointless (and sometimes invalid) to replay via curl.
const SKIP_CURL_HEADERS = new Set(['host', 'content-length', 'connection'])

function buildCurl(entry: RequestEntry): string {
  // -k: the local server may use a self-signed cert on dev machines.
  const parts = [`curl -k -X ${entry.method} ${shSingleQuote(entry.url)}`]
  for (const [k, v] of Object.entries(entry.requestHeaders ?? {})) {
    if (!SKIP_CURL_HEADERS.has(k)) {
      parts.push(`-H ${shSingleQuote(`${k}: ${v}`)}`)
    }
  }
  if (entry.requestBody) {
    parts.push(`--data ${shSingleQuote(entry.requestBody)}`)
  }
  return parts.join(' \\\n  ')
}

function CopyCurlButton({ entry }: { entry: RequestEntry }) {
  const [copied, setCopied] = useState(false)

  const handleCopy = async (): Promise<void> => {
    try {
      await navigator.clipboard.writeText(buildCurl(entry))
      setCopied(true)
      setTimeout(() => setCopied(false), 1500)
    } catch {
      setCopied(false)
    }
  }

  const base =
    'shrink-0 border px-3 py-1.5 text-xs cursor-pointer transition-colors rounded-sm'
  const state = copied
    ? 'border-status-good text-status-good'
    : 'border-grey-300 text-grey-600 hover:border-ocean hover:text-ocean'

  return (
    <button onClick={handleCopy} className={`${base} ${state}`}>
      {copied ? '✓ Copied' : 'Copy as cURL'}
    </button>
  )
}

interface FormattedBody {
  text: string
  language: 'json' | 'text'
}

function formatBody(body: string | undefined, contentType: string | undefined): FormattedBody {
  if (!body) {
    return { text: '', language: 'text' }
  }
  if (contentType && contentType.includes('json')) {
    try {
      return { text: JSON.stringify(JSON.parse(body), null, 2), language: 'json' }
    } catch {
      return { text: body, language: 'text' }
    }
  }
  return { text: body, language: 'text' }
}

function DetailView({ entry }: { entry: RequestEntry }) {
  const formatted = useMemo(
    () => formatBody(entry.responseBody, entry.contentType),
    [entry.responseBody, entry.contentType],
  )

  return (
    <div className="flex flex-col min-h-0 flex-1">
      <div className="px-4 py-3 border-b border-grey-200 space-y-2">
        <div className="flex items-start justify-between gap-3">
          <div className="font-mono text-xs break-all">
            <span className="font-semibold">{entry.method}</span> {entry.url}
          </div>
          <CopyCurlButton entry={entry} />
        </div>
        <div className="text-xs text-grey-700 grid grid-cols-2 gap-x-4 gap-y-0.5">
          <div>
            <span className="text-grey-500">Requested:</span> {formatTime(entry.requestedAt)}
          </div>
          <div>
            <span className="text-grey-500">Completed:</span> {formatTime(entry.completedAt)}
          </div>
          <div>
            <span className="text-grey-500">Duration:</span>{' '}
            {typeof entry.durationMs === 'number' ? `${entry.durationMs.toFixed(1)} ms` : '—'}
          </div>
          <div>
            <span className="text-grey-500">Status:</span>{' '}
            {entry.error
              ? `Error: ${entry.error}`
              : entry.status != null
                ? `${entry.status}${entry.statusText ? ` ${entry.statusText}` : ''}`
                : '—'}
          </div>
          {entry.contentType && (
            <div className="col-span-2">
              <span className="text-grey-500">Content-Type:</span> {entry.contentType}
            </div>
          )}
        </div>
      </div>
      {entry.requestBody && (
        <details className="px-4 py-2 border-b border-grey-200 text-xs">
          <summary className="cursor-pointer text-grey-600">Request body</summary>
          <pre className="mt-2 font-mono whitespace-pre-wrap break-words text-grey-800">
            {entry.requestBody}
          </pre>
        </details>
      )}
      <div className="flex-1 min-h-0 overflow-auto bg-grey-900">
        <pre className="p-4 text-xs font-mono whitespace-pre-wrap break-words text-grey-100 leading-relaxed">
          {formatted.text || (entry.completedAt ? '(empty response)' : 'Pending…')}
        </pre>
      </div>
    </div>
  )
}

export default function RequestsPage() {
  const { entries, clear } = useRequests()
  const [selectedId, setSelectedId] = useState<number | null>(null)
  const [showHealthPolls, setShowHealthPolls] = useState(false)
  const visibleEntries = useMemo(
    () => (showHealthPolls ? entries : entries.filter(e => !isHealthPollUrl(e.url))),
    [entries, showHealthPolls],
  )
  const selected = useMemo(
    () => entries.find(e => e.id === selectedId) ?? null,
    [entries, selectedId],
  )

  return (
    <div className="flex flex-col h-full">
      <PageHeader
        eyebrow="Server"
        title="Requests"
        description="Inspect HTTP requests this app has made to the backend — method, status, timing, headers, and bodies — and copy any request as a cURL command to reproduce it."
      />
      <div className="flex flex-1 min-h-0">
        <div className="w-1/2 border-r border-grey-200 flex flex-col min-h-0">
          <div className="flex items-center justify-between px-4 py-3 border-b border-grey-200 gap-3">
            <p className="text-xs text-grey-600">
              {entries.length} request{entries.length === 1 ? '' : 's'} (max 1000)
            </p>
            <label className="flex items-center gap-1.5 text-xs text-grey-700 cursor-pointer">
              <input
                type="checkbox"
                checked={showHealthPolls}
                onChange={e => setShowHealthPolls(e.target.checked)}
                className="cursor-pointer accent-syn-red"
              />
              Show health polls
            </label>
            <button
              onClick={clear}
              disabled={entries.length === 0}
              className={btnOutlineCls}
            >
              Clear
            </button>
          </div>
          <div className="flex-1 overflow-auto">
            {visibleEntries.length === 0 ? (
              <p className="p-4 text-xs text-grey-500">No requests yet.</p>
            ) : (
              <ul>
                {visibleEntries.map(e => (
                  <li
                    key={e.id}
                    onClick={() => setSelectedId(e.id)}
                    className={`px-4 py-2 border-b border-grey-100 cursor-pointer text-xs font-mono ${
                      selectedId === e.id ? 'bg-syn-red/10' : 'hover:bg-grey-50'
                    }`}
                  >
                    <div className="flex items-center gap-2">
                      <StatusBadge status={e.status} error={e.error} />
                      <span className="font-semibold w-12 inline-block">{e.method}</span>
                      <span className="truncate flex-1">{pathOf(e.url)}</span>
                      {typeof e.durationMs === 'number' && (
                        <span className="text-grey-500 shrink-0">{e.durationMs.toFixed(0)} ms</span>
                      )}
                    </div>
                    <div className="text-grey-500 mt-1">{formatTime(e.requestedAt)}</div>
                  </li>
                ))}
              </ul>
            )}
          </div>
        </div>

        <div className="w-1/2 flex flex-col min-h-0">
          {selected ? (
            <DetailView entry={selected} />
          ) : (
            <p className="p-4 text-xs text-grey-500">Select a request to view its result.</p>
          )}
        </div>
      </div>
    </div>
  )
}
