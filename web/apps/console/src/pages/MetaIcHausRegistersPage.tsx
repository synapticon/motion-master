import { useQuery } from '@tanstack/react-query'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline } from '../utils/styles'

function hex(value: number) {
  return `0x${value.toString(16).toUpperCase().padStart(2, '0')}`
}

// One address, or the range the datasheet prints as a single row.
function addressLabel(address: number, lastAddress: number) {
  return address === lastAddress ? hex(address) : `${hex(address)}–${hex(lastAddress)}`
}

export default function MetaIcHausRegistersPage() {
  const { api } = useConnection()

  const spacesQuery = useQuery({
    queryKey: ['icHausRegisters'],
    queryFn: () => api.getIcHausRegisters(),
    staleTime: Infinity,
  })

  const spaces = spacesQuery.data?.data ?? []

  return (
    <div>
      <PageHeader
        eyebrow="Meta"
        title="iC-Haus Registers"
        description={
          <>
            The register maps of the two iC-Haus chips inside a Circulo's internal encoder — the
            reference for the{' '}
            <span className="font-mono">encoder-register-communication</span> procedure. The{' '}
            <strong>iC-MU</strong> is the position encoder proper, a magnetic off-axis absolute chip
            speaking BiSS-C to the drive; the <strong>iC-PVL</strong> is a battery-buffered Hall
            multiturn counter behind it, reached over I²C through the iC-MU rather than directly. An
            address alone names nothing, so the maps are grouped by <strong>space</strong>: the same
            iC-PVL address <span className="font-mono">0x00</span> is a configuration register in one
            and the status register in another. Each field carries the datasheet's own one-line
            description; <strong>bits</strong> is the field's own slice as the register map prints
            it, not its position in the byte. Transcribed from iC-MU Series Rev B1 and iC-PVL Rev F2;
            the prose behind each one-liner stays in those datasheets.
          </>
        }
      />
      <div className="p-4 sm:px-8 sm:py-7">
        <div className="flex justify-end mb-4">
          <button
            onClick={() => spacesQuery.refetch()}
            disabled={spacesQuery.isFetching}
            className={btnOutline}
          >
            {spacesQuery.isFetching ? 'Loading…' : 'Refresh'}
          </button>
        </div>
        {spacesQuery.isError && (
          <p className="text-xs text-status-bad font-mono">Failed to load the register maps.</p>
        )}
        {spacesQuery.isSuccess && (
          <div className="space-y-7">
            {spaces.map((space) => (
              <section key={`${space.chip} ${space.name}`}>
                <h3 className="font-display uppercase tracking-wide text-sm mb-1">
                  {space.chip} — {space.name}
                </h3>
                <p className="text-xs text-grey-600 mb-3 max-w-3xl">{space.addressing}</p>
                <div className="border border-grey-200 overflow-x-auto">
                  <table className="w-full text-xs border-collapse">
                    <thead>
                      <tr className="border-b border-grey-200 bg-grey-50">
                        {['Address', 'Field', 'Bits', 'Description', 'Reach'].map((h) => (
                          <th
                            key={h}
                            className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap"
                          >
                            {h}
                          </th>
                        ))}
                      </tr>
                    </thead>
                    <tbody>
                      {space.registers.map((r) => {
                        const rows = r.reserved ? [null] : r.fields;
                        return rows.map((field, index) => (
                          <tr
                            key={`${r.address}-${index}`}
                            className={`border-grey-100 ${r.reserved ? 'text-grey-500' : ''} ${
                              // Only the last field of a register closes it off, so the fields of one
                              // register read as one block rather than as unrelated rows.
                              index === rows.length - 1 ? 'border-b last:border-0' : ''
                            }`}
                          >
                            <td className="px-4 py-2 font-mono whitespace-nowrap align-top">
                              {index === 0 ? addressLabel(r.address, r.lastAddress) : ''}
                            </td>
                            <td className="px-4 py-2 font-mono whitespace-nowrap align-top">
                              {field ? field.name : <span className="text-grey-500">reserved</span>}
                            </td>
                            <td className="px-4 py-2 font-mono whitespace-nowrap align-top text-grey-600">
                              {field?.bits || ''}
                            </td>
                            <td className="px-4 py-2 align-top">{field?.description ?? ''}</td>
                            <td className="px-4 py-2 whitespace-nowrap align-top">
                              {index === 0 &&
                                (r.spiOnly ? (
                                  <span
                                    className="text-status-warn"
                                    title="Reachable over SPI only, so the register communication service cannot touch it."
                                  >
                                    SPI only
                                  </span>
                                ) : (
                                  <span className="text-grey-500">—</span>
                                ))}
                            </td>
                          </tr>
                        ));
                      })}
                    </tbody>
                  </table>
                </div>
              </section>
            ))}
          </div>
        )}
      </div>
    </div>
  )
}
