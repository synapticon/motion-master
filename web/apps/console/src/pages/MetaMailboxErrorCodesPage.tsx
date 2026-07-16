import { useQuery } from '@tanstack/react-query'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline } from '../utils/styles'

export default function MetaMailboxErrorCodesPage() {
  const { api } = useConnection()

  const query = useQuery({
    queryKey: ['mailboxErrorCodes'],
    queryFn: () => api.getMailboxErrorCodes(),
    staleTime: Infinity,
  })

  const codes = query.data?.data ?? []

  return (
    <div>
      <PageHeader
        eyebrow="Meta"
        title="Mailbox Error Codes"
        description={
          <>
            CoE mailbox error codes, assembled from{' '}
            <span className="font-mono">ETG.1000.4, Table 30</span> (Error Reply Service Data). A
            slave returns one of these when the mailbox layer — below a specific protocol such as
            CoE or FoE — rejects a transfer; the code is embedded in the error text the mailbox
            endpoints return.
          </>
        }
      />
      <div className="p-4 sm:p-8">
        <div className="flex justify-end mb-4">
          <button
            onClick={() => query.refetch()}
            disabled={query.isFetching}
            className={btnOutline}
          >
            {query.isFetching ? 'Loading…' : 'Refresh'}
          </button>
        </div>
        {query.isError && (
          <p className="text-xs text-status-bad font-mono">Failed to load mailbox error codes.</p>
        )}
        {query.isSuccess && (
          <div className="border border-grey-200 overflow-x-auto">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  {['Code', 'Hex', 'Name', 'Description'].map(h => (
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
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
        {query.isFetching && !query.data && (
          <p className="text-xs text-grey-600">Loading…</p>
        )}
      </div>
    </div>
  )
}
