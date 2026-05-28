import { useQuery } from '@tanstack/react-query'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'

const btnOutlineCls =
  'border border-syn-red text-syn-red px-3 py-1.5 text-xs hover:bg-syn-red hover:text-white disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

export default function AlStatusCodesPage() {
  const { api } = useConnection()

  const query = useQuery({
    queryKey: ['alStatusCodes'],
    queryFn: () => api.getAlStatusCodes(),
    staleTime: Infinity,
  })

  const codes = query.data?.data ?? []

  return (
    <div>
      <PageHeader eyebrow="Meta" title="AL Status Codes" />
      <div className="p-4 sm:p-8">
        <div className="flex justify-end mb-4">
          <button
            onClick={() => query.refetch()}
            disabled={query.isFetching}
            className={btnOutlineCls}
          >
            {query.isFetching ? 'Loading…' : 'Refresh'}
          </button>
        </div>
        {query.isError && (
          <p className="text-xs text-status-bad font-mono">Failed to load AL status codes.</p>
        )}
        {query.isSuccess && (
          <>
            <p className="text-xs text-grey-600 mb-4 max-w-3xl">
              <span className="font-mono text-status-bad">Terminal</span> codes mean the slave cannot reach the
              requested EtherCAT state by retrying — the master must change something (re-init, reflash, power
              cycle) before another transition attempt can succeed. The server abandons such slaves immediately
              during a state transition instead of waiting for the timeout.
            </p>
            <div className="border border-grey-200 overflow-x-auto">
              <table className="w-full text-xs border-collapse">
                <thead>
                  <tr className="border-b border-grey-200 bg-grey-50">
                    {['Code', 'Hex', 'Name', 'Description', 'Terminal'].map(h => (
                      <th key={h} className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">
                        {h}
                      </th>
                    ))}
                  </tr>
                </thead>
                <tbody>
                  {codes.map(c => (
                    <tr key={c.code} className="border-b border-grey-100 last:border-0">
                      <td className="px-4 py-2 font-mono">{c.code}</td>
                      <td className="px-4 py-2 font-mono">0x{c.code.toString(16).toUpperCase().padStart(4, '0')}</td>
                      <td className="px-4 py-2 font-mono">{c.name}</td>
                      <td className="px-4 py-2 text-grey-600">{c.description}</td>
                      <td className="px-4 py-2 font-mono">
                        {c.terminal ? (
                          <span className="text-status-bad">yes</span>
                        ) : (
                          <span className="text-grey-400">—</span>
                        )}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </>
        )}
        {query.isFetching && !query.data && (
          <p className="text-xs text-grey-600">Loading…</p>
        )}
      </div>
    </div>
  )
}
