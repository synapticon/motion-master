import { useEffect, useRef } from 'react'
import { useQuery } from '@tanstack/react-query'
import Callout from '../components/Callout'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { downloadText } from '../utils/download'
import { btnOutline } from '../utils/styles'

export default function ServerLogPage() {
  const { host, httpPort } = useConnection()
  const bottomRef = useRef<HTMLDivElement>(null)

  const logQuery = useQuery({
    queryKey: ['log', host, httpPort],
    queryFn: async () => {
      const res = await fetch(`https://${host}:${httpPort}/api/log`)
      if (!res.ok) throw new Error(`HTTP ${res.status}`)
      return res.text()
    },
    refetchInterval: false,
  })

  const text = logQuery.data ?? ''

  useEffect(() => {
    bottomRef.current?.scrollIntoView()
  }, [text])

  return (
    <div className="flex flex-col h-full">
      <PageHeader
        eyebrow="Server"
        title="Log"
        description="View the backend's diagnostic log — the in-memory ring buffer of server-side events (up to 100 000 entries), useful for troubleshooting fieldbus and API operations."
      />
      <div className="p-4 sm:px-8 sm:py-7 flex flex-col flex-1 min-h-0 space-y-4">
        <Callout variant="info">
          <p>
            This is the in-memory buffer, kept at the server's console verbosity — normally{' '}
            <span className="font-mono">info</span>. It holds the current run only and is lost when
            the server exits.
          </p>
          <p className="mt-1.5 text-grey-600">
            For the full <span className="font-mono">debug</span> log, and for anything from a
            previous run or a crash, download the log file from{' '}
            <strong>Storage → User Cache</strong> (under <span className="font-mono">logs/</span>).
            That is the copy to attach to a bug report.
          </p>
        </Callout>

        <div className="flex items-center justify-end gap-2">
          <button
            onClick={() => downloadText(text, 'motion-master-console.log')}
            disabled={!text}
            className={btnOutline}
          >
            Download
          </button>
          <button
            onClick={() => logQuery.refetch()}
            disabled={logQuery.isFetching}
            className={btnOutline}
          >
            {logQuery.isFetching ? 'Loading…' : 'Refresh'}
          </button>
        </div>
        <div className="flex-1 min-h-0 border border-grey-200 bg-grey-900 overflow-auto">
          {logQuery.isError && (
            <p className="p-4 text-xs text-status-bad font-mono">Failed to load log.</p>
          )}
          {!logQuery.isError && (
            <pre className="p-4 text-xs font-mono text-grey-100 whitespace-pre-wrap break-words leading-relaxed">
              {text || (logQuery.isFetching ? 'Loading…' : 'No log entries.')}
              <div ref={bottomRef} />
            </pre>
          )}
        </div>
      </div>
    </div>
  )
}
