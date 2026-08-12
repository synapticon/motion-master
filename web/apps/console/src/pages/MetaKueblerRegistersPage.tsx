import { useQuery } from '@tanstack/react-query'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline } from '../utils/styles'

const FORMAT_LABEL: Record<string, string> = {
  unsigned: 'Unsigned',
  signed: 'Signed',
  bitField: 'Bit field',
  signedHalves: 'Two signed 16-bit halves',
}

const ACCESS_LABEL: Record<string, string> = {
  ro: 'Read only',
  wo: 'Write only',
  rw: 'Read / write',
}

export default function MetaKueblerRegistersPage() {
  const { api } = useConnection()

  const registersQuery = useQuery({
    queryKey: ['kueblerRegisters'],
    queryFn: () => api.getKueblerRegisters(),
    staleTime: Infinity,
  })

  const registers = registersQuery.data?.data ?? []

  return (
    <div>
      <PageHeader
        eyebrow="Meta"
        title="Kübler Registers"
        description={
          <>
            The register map of the Integro's internal encoder — the reference for the{' '}
            <span className="font-mono">kuebler-register-communication</span> procedure, which reads
            and writes these 1 to 4 bytes at a time. Transcribed from the vendor's own draft table,
            so both of its caveats are carried through: seven registers are documented but{' '}
            <strong>not implemented</strong> by the encoder, and the 64-bit one cannot be transferred
            at all, because the command's length byte caps at four bytes. Format matters as much as
            width — several registers are bit fields whose assembled number means nothing on its own,
            and <span className="font-mono">0x3C</span> is two separate signed values in one
            register.
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
          <p className="text-xs text-status-bad font-mono">Failed to load the register map.</p>
        )}
        {registersQuery.isSuccess && (
          <div className="border border-grey-200 overflow-x-auto">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  {['Address', 'Width', 'Name', 'Access', 'Format', 'Status', 'Bit definitions'].map(
                    (h) => (
                      <th
                        key={h}
                        className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap"
                      >
                        {h}
                      </th>
                    ),
                  )}
                </tr>
              </thead>
              <tbody>
                {registers.map((r) => (
                  <tr
                    key={r.address}
                    className={`border-b border-grey-100 last:border-0 ${r.implemented ? '' : 'text-grey-500'}`}
                  >
                    <td className="px-4 py-2 font-mono whitespace-nowrap">
                      0x{r.address.toString(16).toUpperCase().padStart(2, '0')}
                    </td>
                    <td className="px-4 py-2 font-mono whitespace-nowrap">
                      {r.bits} bit
                      {!r.readableInOneCommand && (
                        <span
                          className="ml-2 text-status-warn"
                          title="Too wide for one command: the length byte caps at 4 bytes."
                        >
                          too wide
                        </span>
                      )}
                    </td>
                    <td className="px-4 py-2">{r.name}</td>
                    <td className="px-4 py-2 whitespace-nowrap">{ACCESS_LABEL[r.access] ?? r.access}</td>
                    <td className="px-4 py-2 whitespace-nowrap">{FORMAT_LABEL[r.format] ?? r.format}</td>
                    <td className="px-4 py-2 whitespace-nowrap">
                      {r.implemented ? (
                        'implemented'
                      ) : (
                        <span className="text-status-warn">not implemented</span>
                      )}
                    </td>
                    <td className="px-4 py-2 text-grey-600">{r.definition}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </div>
    </div>
  )
}
