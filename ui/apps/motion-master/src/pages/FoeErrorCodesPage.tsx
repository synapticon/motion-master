import { useQuery } from '@tanstack/react-query'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'

const btnOutlineCls =
  'border border-syn-red text-syn-red px-3 py-1.5 text-xs hover:bg-syn-red hover:text-white disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

export default function FoeErrorCodesPage() {
  const { api } = useConnection()

  const query = useQuery({
    queryKey: ['foeErrorCodes'],
    queryFn: () => api.getFoeErrorCodes(),
    staleTime: Infinity,
  })

  const codes = query.data?.data ?? []

  return (
    <div>
      <PageHeader eyebrow="Meta" title="FoE Error Codes" />
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
          <p className="text-xs text-status-bad font-mono">Failed to load FoE error codes.</p>
        )}
        {query.isSuccess && (
          <>
            <p className="text-xs text-grey-600 mb-4 max-w-3xl">
              Standard File-over-EtherCAT (FoE) error codes, assembled from{' '}
              <span className="font-mono">ETG.1000.6 §5.8.5, Table 93</span>. Codes in the
              <span className="font-mono"> 0x8000</span> range are standardised by the EtherCAT
              Technology Group; vendor-specific codes fall outside this range and are not listed
              here.
            </p>
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
                      <td className="px-4 py-2 font-mono">0x{c.code.toString(16).toUpperCase().padStart(8, '0')}</td>
                      <td className="px-4 py-2 font-mono">{c.name}</td>
                      <td className="px-4 py-2 text-grey-600">{c.description}</td>
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
