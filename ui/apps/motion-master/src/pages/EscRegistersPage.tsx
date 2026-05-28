import { useQuery } from '@tanstack/react-query'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'

const btnOutlineCls =
  'border border-syn-red text-syn-red px-3 py-1.5 text-xs hover:bg-syn-red hover:text-white disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

export default function EscRegistersPage() {
  const { api } = useConnection()

  const registersQuery = useQuery({
    queryKey: ['registers'],
    queryFn: () => api.getRegisters(),
  })

  const registers = registersQuery.data?.data ?? []

  return (
    <div>
      <PageHeader eyebrow="App" title="Registers" />
      <div className="p-4 sm:p-8">
        <div className="flex justify-end mb-4">
          <button
            onClick={() => registersQuery.refetch()}
            disabled={registersQuery.isFetching}
            className={btnOutlineCls}
          >
            {registersQuery.isFetching ? 'Loading…' : 'Refresh'}
          </button>
        </div>
        {registersQuery.isError && (
          <p className="text-xs text-status-bad font-mono">Failed to load registers.</p>
        )}
        {registersQuery.isSuccess && (
          <div className="border border-grey-200 overflow-x-auto">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  {['Address', 'Hex', 'Length', 'Name', 'Description'].map(h => (
                    <th key={h} className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">
                      {h}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {registers.map(r => (
                  <tr key={r.address} className="border-b border-grey-100 last:border-0">
                    <td className="px-4 py-2 font-mono">{r.address}</td>
                    <td className="px-4 py-2 font-mono">0x{r.address.toString(16).toUpperCase().padStart(4, '0')}</td>
                    <td className="px-4 py-2 font-mono">{r.length}</td>
                    <td className="px-4 py-2 font-mono">{r.name}</td>
                    <td className="px-4 py-2 text-grey-600">{r.description}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
        {registersQuery.isFetching && !registersQuery.data && (
          <p className="text-xs text-grey-600">Loading…</p>
        )}
      </div>
    </div>
  )
}
