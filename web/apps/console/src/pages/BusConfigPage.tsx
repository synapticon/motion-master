import { useQuery } from '@tanstack/react-query'
import type { ReactNode } from 'react'
import { type SlaveConfig, type SyncManagerConfig, type FmmuConfig, formatHex } from '@synapticon/motion-master-client'
import PageHeader from '../components/PageHeader'
import ConfigExplainer from '../components/ConfigExplainer'
import SlavePositionBadge from '../components/SlavePositionBadge'
import { useConnection } from '../contexts/ConnectionContext'
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

// Mailbox protocol bits (SOEM ECT_MBXPROT_*): bit, abbreviation, full name.
const MBX_PROTOCOLS: [number, string, string][] = [
  [0x01, 'AoE', 'ADS over EtherCAT'],
  [0x02, 'EoE', 'Ethernet over EtherCAT'],
  [0x04, 'CoE', 'CANopen over EtherCAT'],
  [0x08, 'FoE', 'File over EtherCAT'],
  [0x10, 'SoE', 'Servo Drive Profile over EtherCAT'],
  [0x20, 'VoE', 'Vendor-specific over EtherCAT'],
]

const decodeProtocols = (bits: number) => MBX_PROTOCOLS.filter(([bit]) => bits & bit)

function Field({ label, value, hint }: { label: string; value: ReactNode; hint?: string }) {
  return (
    <div>
      <p className="text-[10px] uppercase tracking-wide text-grey-500 font-display">{label}</p>
      <p className="font-mono text-sm text-grey-800 mt-0.5">{value}</p>
      {hint && <p className="text-[11px] text-grey-500 mt-1 leading-snug">{hint}</p>}
    </div>
  )
}

function Th({ children, hint }: { children: ReactNode; hint: string }) {
  return (
    <th
      title={hint}
      className="px-4 py-2 font-display uppercase tracking-wide font-medium cursor-help"
    >
      {children}
    </th>
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
            <Th hint="Sync Manager index in the ESC">SM</Th>
            <Th hint="Sync Manager role: mailbox in/out or cyclic process data (RxPDO/TxPDO)">Type</Th>
            <Th hint="Physical ESC memory address the Sync Manager guards">Phys addr</Th>
            <Th hint="Size in bytes of the memory buffer the Sync Manager guards">Length</Th>
            <Th hint="Raw SM control/flags register (buffer mode, direction, watchdog, enable)">
              Flags
            </Th>
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
            <Th hint="Fieldbus Memory Management Unit index in the ESC">FMMU</Th>
            <Th hint="What the FMMU maps: inputs, outputs, or a mailbox Sync Manager's status">
              Type
            </Th>
            <Th hint="Logical (bus-wide) start address and bit range">Logical</Th>
            <Th hint="Size in bytes of the mapped region">Length</Th>
            <Th hint="Physical ESC start address and bit (ties the FMMU to a Sync Manager)">
              Physical
            </Th>
            <Th hint="Whether this FMMU mapping is enabled">Active</Th>
          </tr>
        </thead>
        <tbody>
          {entries.map(f => {
            const type = fmmuType(f)
            const typeHint =
              type === 'Mailbox state'
                ? "A 1-byte flag, not process data: it tells the master when this slave has a mailbox message waiting to be read, so the master only fetches it when there's something there instead of constantly asking."
                : undefined
            return (
            <tr key={f.index} className="border-b border-grey-100 last:border-0">
              <td className="px-4 py-2 font-mono">FMMU{f.index}</td>
              <td className="px-4 py-2 text-grey-700">
                {typeHint ? (
                  <span
                    title={typeHint}
                    className="cursor-help"
                  >
                    {type}
                  </span>
                ) : (
                  type
                )}
              </td>
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
            )
          })}
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
        <SlavePositionBadge position={slave.slavePosition} />
        <span className="text-sm text-grey-800 font-medium">
          {slave.deviceName || <span className="text-grey-400">unknown device</span>}
        </span>
        <span
          className="font-mono text-[11px] text-grey-500 cursor-help"
          title="Station (configured) address assigned during scan"
        >
          @ {formatHex(slave.configuredAddress)}
        </span>
        {slave.aliasAddress > 0 ? (
          <span
            className="font-mono text-[11px] text-grey-500 cursor-help"
            title="Station alias stored in the slave's EEPROM — stable across rescans and recabling, unlike the configured address"
          >
            alias {formatHex(slave.aliasAddress)}
          </span>
        ) : (
          <span
            className="font-mono text-[11px] text-grey-400 cursor-help"
            title="No station alias set in EEPROM. The alias is the only stable per-device identifier across rescans/recabling; the configured (@) address is reassigned every scan."
          >
            alias —
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
              slave.mailbox.writeLength === 0 && slave.mailbox.readLength === 0 ? (
                'none'
              ) : (
                <>
                  {slave.mailbox.writeLength} B @ {formatHex(slave.mailbox.writeOffset)} /{' '}
                  {slave.mailbox.readLength} B @ {formatHex(slave.mailbox.readOffset)}
                </>
              )
            }
            hint="Write (master→slave) / read (slave→master) mailbox windows: length and physical ESC offset of each"
          />
          <Field
            label="Protocols"
            value={
              protocols.length ? (
                protocols.map(([bit, abbr, full], i) => (
                  <span key={bit}>
                    {i > 0 && <span className="text-grey-400"> · </span>}
                    <span
                      title={full}
                      className="cursor-help"
                    >
                      {abbr}
                    </span>
                  </span>
                ))
              ) : (
                '—'
              )
            }
            hint="Mailbox protocols the slave supports"
          />
          <Field
            label="Distributed clock"
            value={
              !slave.dc.capable ? (
                'not capable'
              ) : (
                <>
                  {slave.dc.active ? 'SYNC0' : 'free-run'} · delay {slave.dc.propagationDelay} ns ·
                  cycle {slave.dc.cycleTime} ns · shift {slave.dc.shift} ns
                </>
              )
            }
            hint="DC capability and SYNC0 state, with the master-measured propagation delay and the SYNC0 cycle time / shift. SYNC0 is off for the SM-synchronous bring-up this driver uses, so cycle and shift read 0 until SYNC0 activation is enabled."
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
        <ConfigExplainer />

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
