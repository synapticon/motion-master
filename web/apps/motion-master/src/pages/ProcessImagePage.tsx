import { useQuery } from '@tanstack/react-query'
import type { ProcessImageObject } from '@synapticon/motion-master-client'
import PageHeader from '../components/PageHeader'
import ProcessImageExplainer from '../components/ProcessImageExplainer'
import SlavePositionBadge from '../components/SlavePositionBadge'
import { useConnection } from '../contexts/ConnectionContext'
import { usePreferences } from '../contexts/PreferencesContext'
import { formatHex } from '../utils/hex'
import { btnOutline } from '../utils/styles'

function Stat({
  label,
  value,
  tone,
  hint,
}: {
  label: string
  value: string
  tone?: string
  hint: string
}) {
  const { hintsInline } = usePreferences()
  return (
    <div
      title={hintsInline ? undefined : hint}
      className={`border border-grey-200 px-4 py-3${hintsInline ? '' : ' cursor-help'}`}
    >
      <p className="text-[10px] uppercase tracking-wide text-grey-500 font-display">{label}</p>
      <p className={`font-mono text-lg mt-0.5 ${tone ?? 'text-grey-800'}`}>{value}</p>
      {hintsInline && <p className="text-[11px] leading-snug text-grey-500 mt-1.5">{hint}</p>}
    </div>
  )
}

const COLUMNS = [
  {
    label: 'Slave',
    width: 'w-16',
    hint: 'EtherCAT slave position on the bus (1-based; the master is position 0).',
  },
  {
    label: 'Device',
    width: 'w-48',
    hint: 'Human-readable name of the device at that slave position.',
  },
  {
    label: 'Object',
    width: 'w-28',
    hint: 'Object dictionary entry being mapped, as index:subindex (e.g. 6064:00).',
  },
  {
    label: 'Name',
    width: 'w-auto',
    hint: 'Name of the mapped object dictionary entry, read from the device when its parameters are initialized.',
  },
  {
    label: 'Byte',
    width: 'w-20',
    hint: "Byte offset within this direction's image (bitOffset / 8). Outputs and inputs are exchanged as separate images, so each direction is offset from 0 independently — inputs start at 0 of the input image, not after the outputs.",
  },
  {
    label: 'Bit',
    width: 'w-16',
    hint: 'Bit offset within that byte (bitOffset % 8), for sub-byte packed objects.',
  },
  { label: 'Bits', width: 'w-16', hint: 'Width of the mapped object in bits.' },
]

function MappingTable({
  title,
  entries,
  deviceName,
}: {
  title: string
  entries: ProcessImageObject[]
  deviceName: (pos: number) => string
}) {
  return (
    <section>
      <p className="eyebrow mb-3">
        {title} <span className="text-grey-400">({entries.length})</span>
      </p>
      {entries.length === 0 ? (
        <p className="text-xs text-grey-500">No objects mapped in this direction.</p>
      ) : (
        <div className="border border-grey-200 overflow-x-auto">
          <table className="w-full min-w-[640px] text-xs border-collapse table-fixed">
            <colgroup>
              {COLUMNS.map(c => (
                <col key={c.label} className={c.width} />
              ))}
            </colgroup>
            <thead>
              <tr className="border-b border-grey-200 bg-grey-50">
                {COLUMNS.map(c => (
                  <th
                    key={c.label}
                    title={c.hint}
                    className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap cursor-help"
                  >
                    {c.label}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {entries.map(e => (
                <tr
                  key={`${e.slavePosition}-${e.index}-${e.subindex}-${e.bitOffset}`}
                  className="border-b border-grey-100 last:border-0"
                >
                  <td className="px-4 py-2">
                    <SlavePositionBadge position={e.slavePosition} />
                  </td>
                  <td className="px-4 py-2 text-grey-600 truncate" title={deviceName(e.slavePosition)}>
                    {deviceName(e.slavePosition)}
                  </td>
                  <td className="px-4 py-2 font-mono whitespace-nowrap">
                    {formatHex(e.index)}:{formatHex(e.subindex, 2, false)}
                  </td>
                  <td className="px-4 py-2 text-grey-800 truncate" title={e.name}>
                    {e.name || <span className="text-grey-400">—</span>}
                  </td>
                  <td className="px-4 py-2 font-mono">{Math.floor(e.bitOffset / 8)}</td>
                  <td className="px-4 py-2 font-mono">{e.bitOffset % 8}</td>
                  <td className="px-4 py-2 font-mono">{e.bitLength}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </section>
  )
}

export default function ProcessImagePage() {
  const { api } = useConnection()

  const query = useQuery({
    queryKey: ['processImage'],
    queryFn: () => api.getProcessImage(),
    refetchInterval: 2000,
  })

  // Label each slave position with its device name, when the device list is available.
  const devicesQuery = useQuery({
    queryKey: ['devices'],
    queryFn: () => api.getDevices(),
    staleTime: Infinity,
  })
  const deviceNames = new Map((devicesQuery.data?.data ?? []).map(d => [d.slavePosition, d.name]))
  const deviceName = (pos: number) => deviceNames.get(pos) ?? ''

  const img = query.data?.data

  return (
    <div>
      <PageHeader
        eyebrow="Fieldbus"
        title="Process Image"
        description={
          <>
            The whole-bus EtherCAT process image published for cyclic exchange — what each device
            maps, where it sits in its direction's image, and the working-counter health. Rebuilt
            automatically whenever a device enters or leaves SAFE-OP/OP; each rebuild is a new{' '}
            <span className="font-mono">generation</span>.
          </>
        }
      />
      <div className="p-4 sm:p-8 space-y-8">
        <ProcessImageExplainer />

        <div className="flex justify-end">
          <button
            onClick={() => query.refetch()}
            disabled={query.isFetching}
            className={btnOutline}
          >
            {query.isFetching ? 'Loading…' : 'Refresh'}
          </button>
        </div>

        {query.isError && (
          <p className="text-xs text-status-bad font-mono">Failed to load process image.</p>
        )}

        {query.isFetching && !query.data && <p className="text-xs text-grey-600">Loading…</p>}

        {img && !img.configured && img.generations === 0 && (
          <p className="text-xs text-grey-600">
            No process image has been published. Bring a device to SAFE-OP or OP to map process
            data.
          </p>
        )}

        {img && (img.configured || img.generations > 0) && (
          <>
            {!img.configured && (
              <div className="border border-status-warn/40 bg-status-warn/5 px-4 py-3 text-xs text-grey-700">
                Not currently exchanging — all devices have left SAFE-OP/OP, so the live image was
                torn down. Showing the most recent published image (generation {img.generations}).
                Working-counter health does not apply while the bus is idle.
              </div>
            )}
            <div className="grid grid-cols-2 sm:grid-cols-4 gap-3">
              <Stat
                label="Output bytes"
                value={String(img.outputBytes)}
                hint="Size of the outputs (RxPDO) section of the flat I/O image — data the master writes and sends to the slaves every cycle."
              />
              <Stat
                label="Input bytes"
                value={String(img.inputBytes)}
                hint="Size of the inputs (TxPDO) section of the flat I/O image — data the slaves return to the master every cycle."
              />
              <Stat
                label="Generations"
                value={String(img.generations)}
                hint="How many times the process image has been rebuilt since init. It is remapped each time a device enters or leaves SAFE-OP/OP."
              />
              <Stat
                label="Working counter"
                value={img.configured ? `${img.lastWkc} / ${img.expectedWkc}` : 'Idle'}
                tone={
                  !img.configured
                    ? 'text-grey-400'
                    : img.healthy
                      ? 'text-status-good'
                      : 'text-status-bad'
                }
                hint="EtherCAT working counter: last observed vs expected. Each slave adds 2 for a successful write (outputs) and 1 for a successful read (inputs), so a drive doing both adds 3. A match means every device exchanged; a shortfall means one dropped out. Shows Idle when no device is in SAFE-OP/OP."
              />
            </div>

            <MappingTable title="Outputs · RxPDO" entries={img.outputs} deviceName={deviceName} />
            <MappingTable title="Inputs · TxPDO" entries={img.inputs} deviceName={deviceName} />
          </>
        )}
      </div>
    </div>
  )
}
