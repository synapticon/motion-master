import { useQuery } from '@tanstack/react-query'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline } from '../utils/styles'

export default function MetaEscRegistersPage() {
  const { api } = useConnection()

  const registersQuery = useQuery({
    queryKey: ['registers'],
    queryFn: () => api.getRegisters(),
  })

  const registers = registersQuery.data?.data ?? []

  return (
    <div>
      <PageHeader
        eyebrow="Meta"
        title="ESC Registers"
        description={
          <>
            Well-known EtherCAT Slave Controller (ESC) registers, assembled from the Beckhoff
            EtherCAT ESC datasheet (Section II) and <span className="font-mono">ETG.1000.4</span>.
            FMMU and SyncManager entries list the full block for the first instance; subsequent
            instances follow at fixed strides (FMMU: 16 bytes, SM: 8 bytes).
          </>
        }
      />
      <div className="p-4 sm:px-8 sm:py-7">
        <div className="flex justify-end mb-4">
          <button
            onClick={() => registersQuery.refetch()}
            disabled={registersQuery.isFetching}
            className={btnOutline}
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
