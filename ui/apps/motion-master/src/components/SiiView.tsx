import type { ReactNode } from 'react'
import type { SlaveInformationInterface, SiiPdo } from '@mm/api-client'
import { usePreferences } from '../contexts/PreferencesContext'
import { formatHex } from '../utils/hex'

type Sii = SlaveInformationInterface
type SyncManager = NonNullable<Sii['category']['syncManagers']>[number]
type DistributedClock = NonNullable<Sii['category']['distributedClocks']>[number]

// SII Sync-Manager type (category 41): mirrors the ESC SM role.
const SM_TYPE: Record<number, string> = {
  0: 'Unused',
  1: 'Mailbox Out',
  2: 'Mailbox In',
  3: 'Outputs (RxPDO)',
  4: 'Inputs (TxPDO)',
}

// Mailbox protocol bits (SII standard mailbox protocol word): bit, abbreviation, full name.
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
  const { hintsInline } = usePreferences()
  const tooltip = hint && !hintsInline
  return (
    <div title={tooltip ? hint : undefined} className={tooltip ? 'cursor-help' : undefined}>
      <p className="text-[10px] uppercase tracking-wide text-grey-500 font-display">{label}</p>
      <p className="font-mono text-sm text-grey-800 mt-0.5">{value}</p>
      {hint && hintsInline && <p className="text-[11px] text-grey-500 mt-1 leading-snug">{hint}</p>}
    </div>
  )
}

function Th({ children, hint }: { children: ReactNode; hint?: string }) {
  return (
    <th
      title={hint}
      className={`px-4 py-2 font-display uppercase tracking-wide font-medium ${hint ? 'cursor-help' : ''}`}
    >
      {children}
    </th>
  )
}

function Section({ title, children }: { title: string; children: ReactNode }) {
  return (
    <section className="space-y-2">
      <p className="eyebrow">{title}</p>
      {children}
    </section>
  )
}

const tableWrap = 'border border-grey-200 overflow-x-auto'
const tableCls = 'w-full text-xs border-collapse'
const theadCls = 'border-b border-grey-200 bg-grey-50 text-left text-grey-600'

// SII string indices are 1-based references into the STRINGS table (index 0 means "no string").
function StringRef({ idx, strings }: { idx: number; strings: string[] }) {
  const name = idx >= 1 && idx <= strings.length ? strings[idx - 1] : null
  if (!name) {
    return <span className="text-grey-400">— ({idx})</span>
  }
  return (
    <span>
      {name} <span className="text-grey-400">({idx})</span>
    </span>
  )
}

function PdoTable({ pdos, strings }: { pdos: SiiPdo[]; strings: string[] }) {
  if (pdos.length === 0) {
    return <p className="text-xs text-grey-500">None defined in the EEPROM.</p>
  }
  return (
    <div className="space-y-4">
      {pdos.map((pdo, i) => (
        <div key={`${pdo.pdoIndex}-${i}`} className={tableWrap}>
          <div className="flex flex-wrap items-baseline gap-x-4 gap-y-1 border-b border-grey-200 bg-grey-50 px-4 py-2">
            <span className="font-mono text-sm text-grey-800">{formatHex(pdo.pdoIndex ?? 0)}</span>
            <span className="text-xs text-grey-600">
              <StringRef idx={pdo.nameIdx ?? 0} strings={strings} />
            </span>
            <span
              className="text-[11px] text-grey-500 cursor-help"
              title="Sync Manager this PDO is assigned to in the EEPROM default mapping"
            >
              SM{pdo.syncM} · {pdo.nEntry ?? 0} entr{(pdo.nEntry ?? 0) === 1 ? 'y' : 'ies'}
            </span>
          </div>
          {(pdo.entries?.length ?? 0) > 0 && (
            <table className={`${tableCls} min-w-[520px]`}>
              <thead>
                <tr className={theadCls}>
                  <Th hint="Object dictionary index and subindex of the mapped object">Object</Th>
                  <Th hint="STRINGS-table name of the entry">Name</Th>
                  <Th hint="ETG.1020 data-type code">Data type</Th>
                  <Th hint="Width of the entry in bits">Bits</Th>
                </tr>
              </thead>
              <tbody>
                {pdo.entries!.map((e, j) => (
                  <tr key={j} className="border-b border-grey-100 last:border-0">
                    <td className="px-4 py-2 font-mono">
                      {formatHex(e.entryIndex ?? 0)}:{formatHex(e.subindex ?? 0, 2, false)}
                    </td>
                    <td className="px-4 py-2 text-grey-700">
                      <StringRef idx={e.entryNameIdx ?? 0} strings={strings} />
                    </td>
                    <td className="px-4 py-2 font-mono text-grey-500">
                      {formatHex(e.dataType ?? 0, 4)}
                    </td>
                    <td className="px-4 py-2 font-mono">{e.bitLen ?? 0}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </div>
      ))}
    </div>
  )
}

// Renders a parsed SII image: the identity/mailbox header followed by the decoded categories
// (strings, general, default Sync Managers / FMMUs / PDOs, distributed clocks). Shared by the
// device SII page (read from hardware) and the Tools SII page (parsed from an uploaded file).
export default function SiiView({ sii }: { sii: SlaveInformationInterface }) {
  const { info, category } = sii
  const strings = category.strings ?? []
  const general = category.general
  const syncManagers = category.syncManagers ?? []
  const fmmus = category.fmmus ?? []
  const rxPdos = category.rxPdos ?? []
  const txPdos = category.txPdos ?? []
  const distributedClocks = category.distributedClocks ?? []

  return (
    <div className="space-y-8">
      {/* Identity & mailbox header */}
      <section className="border border-grey-200">
        <header className="border-b border-grey-200 bg-grey-50 px-4 py-3">
          <span className="text-sm text-grey-800 font-medium">
            {general && strings.length > 0 ? (
              <StringRef idx={general.nameIdx ?? 0} strings={strings} />
            ) : (
              'Header'
            )}
          </span>
        </header>
        <div className="p-4 grid grid-cols-2 sm:grid-cols-4 gap-4">
          <Field label="Vendor ID" value={formatHex(info.vendorId ?? 0, 8)} hint="Manufacturer ID from the EEPROM" />
          <Field label="Product code" value={formatHex(info.productCode ?? 0, 8)} hint="Product code from the EEPROM" />
          <Field label="Revision" value={formatHex(info.revisionNumber ?? 0, 8)} hint="Revision number from the EEPROM" />
          <Field label="Serial" value={String(info.serialNumber ?? 0)} hint="Serial number from the EEPROM" />
          <Field
            label="Station alias"
            value={formatHex(info.configuredStationAlias ?? 0)}
            hint="Configured station alias stored in the EEPROM"
          />
          <Field
            label="Mailbox (rx / tx)"
            value={`${info.standardReceiveMailboxSize ?? 0} / ${info.standardSendMailboxSize ?? 0} B`}
            hint="Standard mailbox sizes: receive (master→slave) / send (slave→master)"
          />
          <Field
            label="Protocols"
            value={
              decodeProtocols(info.mailboxProtocol ?? 0).length ? (
                decodeProtocols(info.mailboxProtocol ?? 0).map(([bit, abbr, full], i) => (
                  <span key={bit}>
                    {i > 0 && <span className="text-grey-400"> · </span>}
                    <span title={full} className="cursor-help">
                      {abbr}
                    </span>
                  </span>
                ))
              ) : (
                '—'
              )
            }
            hint="Mailbox protocols declared in the EEPROM"
          />
          <Field
            label="EEPROM size"
            value={`${((info.size ?? 0) + 1) / 8} KB`}
            hint="Declared EEPROM size (the raw 'size' word is KiBit − 1)"
          />
        </div>
      </section>

      {/* Strings */}
      <Section title="Strings">
        {strings.length === 0 ? (
          <p className="text-xs text-grey-500">No string table in the EEPROM.</p>
        ) : (
          <div className={tableWrap}>
            <table className={`${tableCls} min-w-[320px]`}>
              <thead>
                <tr className={theadCls}>
                  <Th hint="1-based index referenced by other categories">Index</Th>
                  <Th>Value</Th>
                </tr>
              </thead>
              <tbody>
                {strings.map((s, i) => (
                  <tr key={i} className="border-b border-grey-100 last:border-0">
                    <td className="px-4 py-2 font-mono text-grey-500">{i + 1}</td>
                    <td className="px-4 py-2 text-grey-800">{s}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Section>

      {/* General */}
      {general && (
        <Section title="General">
          <div className="border border-grey-200 p-4 grid grid-cols-2 sm:grid-cols-4 gap-4">
            <Field label="Group" value={<StringRef idx={general.groupIdx ?? 0} strings={strings} />} hint="STRINGS-table group name" />
            <Field label="Name" value={<StringRef idx={general.nameIdx ?? 0} strings={strings} />} hint="STRINGS-table device name" />
            <Field label="Order" value={<StringRef idx={general.orderIdx ?? 0} strings={strings} />} hint="STRINGS-table order number" />
            <Field label="Image" value={<StringRef idx={general.imgIdx ?? 0} strings={strings} />} hint="STRINGS-table image name" />
            <Field label="CoE details" value={formatHex(general.coeDetails ?? 0, 2)} hint="CoE capability flags" />
            <Field label="FoE details" value={formatHex(general.foeDetails ?? 0, 2)} hint="FoE capability flags" />
            <Field label="EoE details" value={formatHex(general.eoeDetails ?? 0, 2)} hint="EoE capability flags" />
            <Field label="Flags" value={formatHex(general.flags ?? 0, 2)} hint="General device flags" />
            <Field label="DS402 channels" value={String(general.ds402Channels ?? 0)} hint="Number of DS402 channels" />
            <Field label="E-Bus current" value={`${general.currentOnEBus ?? 0} mA`} hint="Current consumption on the E-Bus" />
          </div>
        </Section>
      )}

      {/* Sync Managers */}
      <Section title="Sync Managers (default)">
        {syncManagers.length === 0 ? (
          <p className="text-xs text-grey-500">No Sync Managers in the EEPROM.</p>
        ) : (
          <div className={tableWrap}>
            <table className={`${tableCls} min-w-[520px]`}>
              <thead>
                <tr className={theadCls}>
                  <Th hint="Sync Manager role declared in the EEPROM">Type</Th>
                  <Th hint="Physical ESC memory address the Sync Manager guards">Phys addr</Th>
                  <Th hint="Size in bytes of the guarded buffer">Length</Th>
                  <Th hint="Raw SM control register byte">Control</Th>
                  <Th hint="Whether the Sync Manager is enabled by default">Enabled</Th>
                </tr>
              </thead>
              <tbody>
                {syncManagers.map((sm: SyncManager, i: number) => (
                  <tr key={i} className="border-b border-grey-100 last:border-0">
                    <td className="px-4 py-2 text-grey-700">
                      {SM_TYPE[sm.syncManagerType ?? 0] ?? `Type ${sm.syncManagerType}`}
                    </td>
                    <td className="px-4 py-2 font-mono">{formatHex(sm.physicalStartAddress ?? 0)}</td>
                    <td className="px-4 py-2 font-mono">{sm.length ?? 0} B</td>
                    <td className="px-4 py-2 font-mono text-grey-500">{formatHex(sm.controlRegister ?? 0, 2)}</td>
                    <td className="px-4 py-2">
                      {sm.enableSyncManager ? (
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
        )}
      </Section>

      {/* FMMUs */}
      <Section title="FMMUs (default)">
        {fmmus.length === 0 ? (
          <p className="text-xs text-grey-500">No FMMUs in the EEPROM.</p>
        ) : (
          <div className="flex flex-wrap gap-2">
            {fmmus.map((f, i) => (
              <span
                key={i}
                className="border border-grey-200 px-3 py-1.5 text-xs font-mono"
                title="Raw FMMU configuration word from the EEPROM"
              >
                FMMU{i}: {formatHex(f, 4)}
              </span>
            ))}
          </div>
        )}
      </Section>

      {/* PDOs */}
      <Section title="RxPDOs (default, master→slave)">
        <PdoTable pdos={rxPdos} strings={strings} />
      </Section>
      <Section title="TxPDOs (default, slave→master)">
        <PdoTable pdos={txPdos} strings={strings} />
      </Section>

      {/* Distributed clocks */}
      {distributedClocks.length > 0 && (
        <Section title="Distributed Clocks">
          <div className={tableWrap}>
            <table className={`${tableCls} min-w-[520px]`}>
              <thead>
                <tr className={theadCls}>
                  <Th hint="STRINGS-table name of the DC setting">Name</Th>
                  <Th hint="SYNC0 cycle time in nanoseconds">Cycle 0</Th>
                  <Th hint="SYNC0 shift time in nanoseconds">Shift 0</Th>
                  <Th hint="SYNC1 shift time in nanoseconds">Shift 1</Th>
                  <Th hint="Raw DC assign/activate register value">Activate</Th>
                </tr>
              </thead>
              <tbody>
                {distributedClocks.map((dc: DistributedClock, i: number) => (
                  <tr key={i} className="border-b border-grey-100 last:border-0">
                    <td className="px-4 py-2 text-grey-700">
                      <StringRef idx={dc.nameIdx ?? 0} strings={strings} />
                    </td>
                    <td className="px-4 py-2 font-mono">{dc.cycleTime0 ?? 0} ns</td>
                    <td className="px-4 py-2 font-mono">{dc.shiftTime0 ?? 0} ns</td>
                    <td className="px-4 py-2 font-mono">{dc.shiftTime1 ?? 0} ns</td>
                    <td className="px-4 py-2 font-mono text-grey-500">{formatHex(dc.assignActivate ?? 0, 4)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </Section>
      )}
    </div>
  )
}
