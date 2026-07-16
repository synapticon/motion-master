import { useQuery } from '@tanstack/react-query'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline } from '../utils/styles'

export default function MetaSdoAbortCodesPage() {
  const { api } = useConnection()

  const query = useQuery({
    queryKey: ['sdoAbortCodes'],
    queryFn: () => api.getSdoAbortCodes(),
    staleTime: Infinity,
  })

  const codes = query.data?.data ?? []

  return (
    <div>
      <PageHeader
        eyebrow="Meta"
        title="SDO Abort Codes"
        description={
          <>
            CoE SDO abort codes, assembled from{' '}
            <span className="font-mono">ETG.1000.6 §5.6.2.7.2, Table 41</span> (which reproduces the
            CANopen <span className="font-mono">CiA 301</span> abort transfer codes). A slave returns
            one of these when a mailbox SDO read or write fails; the code is embedded in the error
            text the SDO endpoints return.
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
          <p className="text-xs text-status-bad font-mono">Failed to load SDO abort codes.</p>
        )}
        {query.isSuccess && (
          <div className="border border-grey-200 overflow-x-auto">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  {['Code', 'Hex', 'Description'].map(h => (
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
                    <td className="px-4 py-2 font-mono">0x{c.code.toString(16).toUpperCase().padStart(8, '0')}</td>
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
