import { useQuery } from '@tanstack/react-query'
import type { SlaveConfig, SyncManagerConfig, FmmuConfig } from '@mm/api-client'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { formatHex } from '../utils/hex'
import { btnOutline } from '../utils/styles'

// SOEM SMtype: 0 unused, 1 MbxOut, 2 MbxIn, 3 Outputs, 4 Inputs.
const SM_TYPE: Record<number, string> = {
  0: 'Unused',
  1: 'Mailbox Out',
  2: 'Mailbox In',
  3: 'Outputs (RxPDO)',
  4: 'Inputs (TxPDO)',
}

// ESC FMMU Type register (reg 0x060B): bit0 = read (Inputs/TxPDO), bit1 = write (Outputs/RxPDO).
const FMMU_TYPE: Record<number, string> = {
  0: 'Unused',
  1: 'Inputs',
  2: 'Outputs',
  3: 'SM status',
}

// SyncManager configuration-register block in the ESC (8 bytes per SM from 0x0800). An FMMU whose
// physical target falls here maps a mailbox SM's status byte into logical space so the master can
// poll mailbox-full state via the cyclic LRW datagram — it is not process data.
const SM_REGISTER_BLOCK_START = 0x0800
const SM_REGISTER_BLOCK_END = 0x0880

const fmmuType = (f: FmmuConfig): string => {
  if (f.physicalStart >= SM_REGISTER_BLOCK_START && f.physicalStart < SM_REGISTER_BLOCK_END) {
    return 'Mailbox state'
  }
  return FMMU_TYPE[f.type] ?? `Type ${f.type}`
}

// Mailbox protocol bits (SOEM ECT_MBXPROT_*).
const MBX_PROTOCOLS: [number, string][] = [
  [0x01, 'AoE'],
  [0x02, 'EoE'],
  [0x04, 'CoE'],
  [0x08, 'FoE'],
  [0x10, 'SoE'],
  [0x20, 'VoE'],
]

const decodeProtocols = (bits: number) =>
  MBX_PROTOCOLS.filter(([bit]) => bits & bit).map(([, name]) => name)

function Field({ label, value, hint }: { label: string; value: string; hint?: string }) {
  return (
    <div title={hint} className={hint ? 'cursor-help' : undefined}>
      <p className="text-[10px] uppercase tracking-wide text-grey-500 font-display">{label}</p>
      <p className="font-mono text-sm text-grey-800 mt-0.5">{value}</p>
    </div>
  )
}

function SyncManagerTable({ entries }: { entries: SyncManagerConfig[] }) {
  if (entries.length === 0) {
    return <p className="text-xs text-grey-500">No Sync Managers configured.</p>
  }
  return (
    <div className="border border-grey-200 overflow-x-auto">
      <table className="w-full min-w-[480px] text-xs border-collapse">
        <thead>
          <tr className="border-b border-grey-200 bg-grey-50 text-left text-grey-600">
            <th className="px-4 py-2 font-display uppercase tracking-wide font-medium">SM</th>
            <th className="px-4 py-2 font-display uppercase tracking-wide font-medium">Type</th>
            <th
              className="px-4 py-2 font-display uppercase tracking-wide font-medium cursor-help"
              title="Physical ESC memory address the Sync Manager guards"
            >
              Phys addr
            </th>
            <th className="px-4 py-2 font-display uppercase tracking-wide font-medium">Length</th>
            <th
              className="px-4 py-2 font-display uppercase tracking-wide font-medium cursor-help"
              title="Raw SM control/flags register (buffer mode, direction, watchdog, enable)"
            >
              Flags
            </th>
          </tr>
        </thead>
        <tbody>
          {entries.map(sm => (
            <tr key={sm.index} className="border-b border-grey-100 last:border-0">
              <td className="px-4 py-2 font-mono">SM{sm.index}</td>
              <td className="px-4 py-2 text-grey-700">{SM_TYPE[sm.type] ?? `Type ${sm.type}`}</td>
              <td className="px-4 py-2 font-mono">{formatHex(sm.physicalStart)}</td>
              <td className="px-4 py-2 font-mono">{sm.length} B</td>
              <td className="px-4 py-2 font-mono text-grey-500">{formatHex(sm.flags, 8)}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

function FmmuTable({ entries }: { entries: FmmuConfig[] }) {
  if (entries.length === 0) {
    return <p className="text-xs text-grey-500">No FMMUs configured.</p>
  }
  return (
    <div className="border border-grey-200 overflow-x-auto">
      <table className="w-full min-w-[560px] text-xs border-collapse">
        <thead>
          <tr className="border-b border-grey-200 bg-grey-50 text-left text-grey-600">
            <th className="px-4 py-2 font-display uppercase tracking-wide font-medium">FMMU</th>
            <th className="px-4 py-2 font-display uppercase tracking-wide font-medium">Type</th>
            <th
              className="px-4 py-2 font-display uppercase tracking-wide font-medium cursor-help"
              title="Logical (bus-wide) start address and bit range"
            >
              Logical
            </th>
            <th className="px-4 py-2 font-display uppercase tracking-wide font-medium">Length</th>
            <th
              className="px-4 py-2 font-display uppercase tracking-wide font-medium cursor-help"
              title="Physical ESC start address and bit (ties the FMMU to a Sync Manager)"
            >
              Physical
            </th>
            <th className="px-4 py-2 font-display uppercase tracking-wide font-medium">Active</th>
          </tr>
        </thead>
        <tbody>
          {entries.map(f => (
            <tr key={f.index} className="border-b border-grey-100 last:border-0">
              <td className="px-4 py-2 font-mono">FMMU{f.index}</td>
              <td className="px-4 py-2 text-grey-700">{fmmuType(f)}</td>
              <td className="px-4 py-2 font-mono">
                {formatHex(f.logicalStart, 8)}.{f.logicalStartBit}–{f.logicalEndBit}
              </td>
              <td className="px-4 py-2 font-mono">{f.length} B</td>
              <td className="px-4 py-2 font-mono">
                {formatHex(f.physicalStart)}.{f.physicalStartBit}
              </td>
              <td className="px-4 py-2">
                {f.active ? (
                  <span className="text-status-good">yes</span>
                ) : (
                  <span className="text-grey-400">no</span>
                )}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

function SlaveCard({ slave }: { slave: SlaveConfig }) {
  const protocols = decodeProtocols(slave.mailbox.protocols)
  return (
    <section className="border border-grey-200">
      <header className="border-b border-grey-200 bg-grey-50 px-4 py-3 flex flex-wrap items-baseline gap-x-3 gap-y-1">
        <span className="font-mono text-sm text-grey-800">#{slave.slavePosition}</span>
        <span className="text-sm text-grey-800 font-medium">
          {slave.deviceName || <span className="text-grey-400">unknown device</span>}
        </span>
        <span
          className="font-mono text-[11px] text-grey-500 cursor-help"
          title="Station (configured) address assigned during scan"
        >
          @ {formatHex(slave.configuredAddress)}
        </span>
        {slave.aliasAddress > 0 && (
          <span className="font-mono text-[11px] text-grey-500" title="Configured station alias">
            alias {formatHex(slave.aliasAddress)}
          </span>
        )}
      </header>

      <div className="p-4 space-y-5">
        <div className="grid grid-cols-2 sm:grid-cols-4 gap-4">
          <Field
            label="Output / Input"
            value={`${slave.outputBits} / ${slave.inputBits} bits`}
            hint="Mapped process-data bits per direction (master→slave / slave→master)"
          />
          <Field
            label="Mailbox"
            value={
              slave.mailbox.writeLength === 0 && slave.mailbox.readLength === 0
                ? 'none'
                : `${slave.mailbox.writeLength} / ${slave.mailbox.readLength} B`
            }
            hint="Write (master→slave) / read (slave→master) mailbox lengths"
          />
          <Field
            label="Protocols"
            value={protocols.length ? protocols.join(' · ') : '—'}
            hint="Mailbox protocols the slave supports"
          />
          <Field
            label="Distributed clock"
            value={
              !slave.dc.capable
                ? 'not capable'
                : slave.dc.active
                  ? `SYNC0, ${slave.dc.propagationDelay} ns`
                  : `free-run, ${slave.dc.propagationDelay} ns`
            }
            hint="DC capability and SYNC0 state; propagation delay measured by ecx_configdc. SYNC0 is off for SM-synchronous bring-up."
          />
        </div>

        <div className="space-y-2">
          <p className="eyebrow">Sync Managers</p>
          <SyncManagerTable entries={slave.syncManagers} />
        </div>

        <div className="space-y-2">
          <p className="eyebrow">FMMUs</p>
          <FmmuTable entries={slave.fmmus} />
        </div>
      </div>
    </section>
  )
}

export default function BusConfigPage() {
  const { api } = useConnection()

  const query = useQuery({
    queryKey: ['busConfig'],
    queryFn: () => api.getBusConfig(),
  })

  const slaves = query.data?.data ?? []

  return (
    <div>
      <PageHeader
        eyebrow="Fieldbus"
        title="Configuration"
        description={
          <>
            The static EtherCAT Slave Controller configuration the master programmed during the
            last scan — addresses, mailbox, distributed clock, and the Sync Managers and FMMUs that
            place each slave's process data on the bus. Read from cached state with no bus I/O.
          </>
        }
      />
      <div className="p-4 sm:p-8 space-y-6">
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
          <p className="text-xs text-status-bad font-mono">Failed to load bus configuration.</p>
        )}

        {query.isFetching && !query.data && <p className="text-xs text-grey-600">Loading…</p>}

        {query.data && slaves.length === 0 && (
          <p className="text-xs text-grey-600">
            No slaves configured. Scan the bus to enumerate and configure devices.
          </p>
        )}

        {slaves.map(slave => (
          <SlaveCard key={slave.slavePosition} slave={slave} />
        ))}
      </div>
    </div>
  )
}
