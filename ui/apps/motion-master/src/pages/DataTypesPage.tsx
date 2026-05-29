import { useQuery } from '@tanstack/react-query'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'

const btnOutlineCls =
  'border border-syn-red text-syn-red px-3 py-1.5 text-xs hover:bg-syn-red hover:text-white disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

export default function DataTypesPage() {
  const { api } = useConnection()

  const query = useQuery({
    queryKey: ['dataTypes'],
    queryFn: () => api.getDataTypes(),
    staleTime: Infinity,
  })

  const types = query.data?.data ?? []

  return (
    <div>
      <PageHeader
        eyebrow="Meta"
        title="Object Data Types"
        description={
          <>
            CANopen-over-EtherCAT (CoE) object dictionary data type codes, assembled from{' '}
            <span className="font-mono">ETG.1020 §4.1.7</span> (Data Types) with{' '}
            <span className="font-mono">ETG.5001</span> / <span className="font-mono">ETG.1000</span>{' '}
            cross-references. The code is the 16-bit DataType field returned when reading an object's
            metadata from the dictionary.
          </>
        }
      />
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
          <p className="text-xs text-status-bad font-mono">Failed to load data types.</p>
        )}
        {query.isSuccess && (
          <div className="border border-grey-200 overflow-x-auto">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  {['Code', 'Hex', 'Name', 'Bit Size'].map(h => (
                    <th key={h} className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">
                      {h}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {types.map(t => (
                  <tr key={t.code} className="border-b border-grey-100 last:border-0">
                    <td className="px-4 py-2 font-mono">{t.code}</td>
                    <td className="px-4 py-2 font-mono">0x{t.code.toString(16).toUpperCase().padStart(4, '0')}</td>
                    <td className="px-4 py-2 font-mono">{t.name}</td>
                    <td className="px-4 py-2 font-mono">{t.bitSize === 0 ? '—' : t.bitSize}</td>
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
