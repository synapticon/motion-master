import { useState } from 'react'
import type { ReactNode } from 'react'
import { ChevronDown } from 'lucide-react'
import {
  formatHex,
  type EniCoeCmd,
  type EniInitCmd,
  type EniParseResult,
  type EniSlave,
} from '@synapticon/motion-master-client'

// Renders a parsed ENI. The document is a script of EtherCAT datagrams, so the centre of this view
// is the init-command table: what each one does, which register it touches, and what its payload
// means. Everything the server could work out has already been worked out — the annotations come
// from `POST /api/eni/parse`, which decodes them with the same ESC knowledge the exporter uses to
// build them.

function Th({ children, hint }: { children: ReactNode; hint: string }) {
  return (
    <th
      title={hint}
      className="px-3 py-2 font-display uppercase tracking-wide font-medium cursor-help"
    >
      {children}
    </th>
  )
}

function Field({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div>
      <dt className="eyebrow text-grey-500">{label}</dt>
      <dd className="font-mono text-xs text-grey-800 mt-0.5">{children}</dd>
    </div>
  )
}

function Section({
  title,
  subtitle,
  defaultOpen = false,
  children,
}: {
  title: string
  subtitle?: ReactNode
  defaultOpen?: boolean
  children: ReactNode
}) {
  const [open, setOpen] = useState(defaultOpen)
  return (
    <section className="border border-grey-200">
      <button
        type="button"
        onClick={() => setOpen(o => !o)}
        aria-expanded={open}
        className="w-full flex items-center justify-between gap-3 px-4 py-3 text-left hover:bg-grey-50 transition-colors cursor-pointer"
      >
        <span className="flex items-baseline gap-3 min-w-0">
          <span className="eyebrow shrink-0">{title}</span>
          {subtitle && <span className="text-xs text-grey-500 truncate">{subtitle}</span>}
        </span>
        <ChevronDown
          aria-hidden="true"
          className={`w-4 h-4 text-grey-500 shrink-0 transition-transform ${open ? 'rotate-180' : ''}`}
        />
      </button>
      {open && <div className="border-t border-grey-200 px-4 py-4 space-y-4">{children}</div>}
    </section>
  )
}

/// Says in one line what a datagram does, using whatever the server managed to decode. A command
/// nobody could annotate falls back to its own comment, which is what the writing tool chose to say.
function meaning(cmd: EniInitCmd): ReactNode {
  const register = cmd.register
  if (!register) {
    return <span className="text-grey-400">{cmd.comment ?? '—'}</span>
  }
  if (register.requestsState) {
    return <span className="text-grey-800">request {register.requestsState}</span>
  }
  if (register.waitsForState) {
    return <span className="text-grey-800">wait for {register.waitsForState}</span>
  }
  if (register.decoded) {
    return (
      <span className="text-grey-700">
        {Object.entries(register.decoded)
          .map(([key, value]) => {
            if (typeof value === 'boolean') {
              return value ? key : `not ${key}`
            }
            if (typeof value === 'number' && (key.includes('tart') || key.includes('ddr'))) {
              return `${key} ${formatHex(value)}`
            }
            return `${key} ${value}`
          })
          .join(', ')}
      </span>
    )
  }
  return <span className="text-grey-400">{cmd.comment ?? '—'}</span>
}

/// Splits a process-image variable name into the scope its writer prefixed and the signal itself.
///
/// The ENI gives a variable one name and no structure, and every tool builds it the same way: the
/// device label, a dot, then the signal — `Device 1.Controlword`, `Term 4 (EL5001).Kanal 1.Value`.
/// Splitting at the first dot separates the two, and a name with no dot is all signal.
function splitVariableName(name: string): { scope: string | null; signal: string } {
  const dot = name.indexOf('.')
  if (dot < 0) {
    return { scope: null, signal: name }
  }
  return { scope: name.slice(0, dot), signal: name.slice(dot + 1) }
}

/// Finds which device owns a bit offset in one half of the process image.
///
/// The name's prefix says which device a tool *called* it; this says which device's window the
/// value actually lands in, which is the document's own answer and does not depend on a naming
/// convention. Outputs are matched against each device's `send` window and inputs against `recv`,
/// both named from the master's side.
function deviceAt(slaves: EniSlave[], half: 'outputs' | 'inputs', bitOffs: number): number | null {
  for (let i = 0; i < slaves.length; i += 1) {
    const window =
      half === 'outputs' ? slaves[i].processData?.send : slaves[i].processData?.recv
    if (window && bitOffs >= window.bitStart && bitOffs < window.bitStart + window.bitLength) {
      return i + 1
    }
  }
  return null
}

function InitCmdTable({ cmds }: { cmds: EniInitCmd[] }) {
  if (cmds.length === 0) {
    return <p className="text-xs text-grey-500">No init commands.</p>
  }
  return (
    <div className="border border-grey-200 overflow-x-auto">
      <table className="w-full min-w-[720px] text-xs border-collapse">
        <thead>
          <tr className="border-b border-grey-200 bg-grey-50 text-left text-grey-600">
            <Th hint="When the command is sent: the AL-state transitions of an init command, e.g. IP is INIT to PRE-OP, or the AL states a cyclic command runs in">
              At
            </Th>
            <Th hint="EtherCAT command type. The addressing mode is part of it: AP by ring position, FP by station address, B to every device, L to logical memory">
              Cmd
            </Th>
            <Th hint="Device address (Adp) and ESC memory offset (Ado), or the logical address for an L command">
              Address
            </Th>
            <Th hint="The ESC register that offset selects, named from the register catalogue">
              Register
            </Th>
            <Th hint="What the payload means once decoded, where this project knows how to decode it">
              Meaning
            </Th>
            <Th hint="Payload as the document writes it, or the byte count for a read">Data</Th>
          </tr>
        </thead>
        <tbody>
          {cmds.map((cmd, i) => (
            <tr key={i} className="border-b border-grey-100 last:border-0 align-top">
              {/* An init command names the transitions it runs at; a cyclic one names the AL
                  states it is sent in. The column shows whichever the command has. */}
              <td className="px-3 py-2 font-mono">
                {(cmd.transitions ?? cmd.states ?? []).join(' ')}
              </td>
              <td className="px-3 py-2 font-mono">{cmd.cmdName}</td>
              <td className="px-3 py-2 font-mono text-grey-600">
                {cmd.addr !== undefined
                  ? formatHex(cmd.addr, 8)
                  : `${cmd.adp !== undefined ? formatHex(cmd.adp) : '—'} : ${
                      cmd.ado !== undefined ? formatHex(cmd.ado) : '—'
                    }`}
              </td>
              <td className="px-3 py-2 font-mono text-grey-700" title={cmd.register?.description}>
                {cmd.register?.name ?? <span className="text-grey-400">—</span>}
              </td>
              <td className="px-3 py-2">{meaning(cmd)}</td>
              <td className="px-3 py-2 font-mono text-grey-500 break-all">
                {cmd.data ? cmd.data.hex : cmd.dataLength !== undefined ? `${cmd.dataLength} B` : '—'}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

function CoeCmdTable({ cmds }: { cmds: EniCoeCmd[] }) {
  return (
    <div className="border border-grey-200 overflow-x-auto">
      <table className="w-full min-w-[560px] text-xs border-collapse">
        <thead>
          <tr className="border-b border-grey-200 bg-grey-50 text-left text-grey-600">
            <Th hint="AL-state transitions this transfer runs at">At</Th>
            <Th hint="Whether the master writes the object (download) or reads it (upload)">
              Direction
            </Th>
            <Th hint="Object index and subindex in the device's CoE dictionary">Object</Th>
            <Th hint="Payload, least-significant byte first as CoE writes it">Data</Th>
            <Th hint="What the writing tool said this transfer is for">Comment</Th>
          </tr>
        </thead>
        <tbody>
          {cmds.map((cmd, i) => (
            <tr key={i} className="border-b border-grey-100 last:border-0">
              <td className="px-3 py-2 font-mono">{cmd.transitions.join(' ')}</td>
              <td className="px-3 py-2">{cmd.ccsName}</td>
              <td className="px-3 py-2 font-mono">
                {formatHex(cmd.index)}:{String(cmd.subindex).padStart(2, '0')}
              </td>
              <td className="px-3 py-2 font-mono text-grey-500 break-all">
                {cmd.data?.hex ?? '—'}
              </td>
              <td className="px-3 py-2 text-grey-600">{cmd.comment ?? '—'}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

function SlaveCard({ slave, position }: { slave: EniSlave; position: number }) {
  const previous = slave.previousPorts?.find(p => p.selected)
  return (
    <Section
      title={`Device ${position}`}
      subtitle={`${slave.info.name} · ${formatHex(slave.info.physAddr)} · ${slave.initCmds.length} commands`}
    >
      <dl className="grid grid-cols-2 sm:grid-cols-4 gap-4">
        <Field label="Station address">{formatHex(slave.info.physAddr)}</Field>
        <Field label="Auto-increment">{formatHex(slave.info.autoIncAddr)}</Field>
        <Field label="Vendor">{formatHex(slave.info.vendorId, 8)}</Field>
        <Field label="Product">{formatHex(slave.info.productCode, 8)}</Field>
        <Field label="Revision">{formatHex(slave.info.revisionNo, 8)}</Field>
        <Field label="Serial">{slave.info.serialNo}</Field>
        <Field label="Ports">{slave.info.physics || '—'}</Field>
        <Field label="Plugged into">
          {previous
            ? `port ${previous.port}${previous.physAddr !== undefined ? ` of ${formatHex(previous.physAddr)}` : ''}`
            : 'first on the bus'}
        </Field>
      </dl>

      {slave.processData && (
        <div className="space-y-2">
          <p className="eyebrow text-grey-500">Process data</p>
          <dl className="grid grid-cols-2 sm:grid-cols-4 gap-4">
            {/* Send and Recv are named from the master's side: send is its output image. */}
            <Field label="Outputs">
              {slave.processData.send
                ? `${slave.processData.send.bitLength} bits @ ${slave.processData.send.bitStart}`
                : 'none'}
            </Field>
            <Field label="Inputs">
              {slave.processData.recv
                ? `${slave.processData.recv.bitLength} bits @ ${slave.processData.recv.bitStart}`
                : 'none'}
            </Field>
          </dl>
          {slave.processData.syncManagers.length > 0 && (
            <div className="border border-grey-200 overflow-x-auto">
              <table className="w-full min-w-[480px] text-xs border-collapse">
                <thead>
                  <tr className="border-b border-grey-200 bg-grey-50 text-left text-grey-600">
                    <Th hint="Sync Manager channel number in the ESC">SM</Th>
                    <Th hint="What the channel carries: a mailbox, or process data in one direction">
                      Type
                    </Th>
                    <Th hint="Physical ESC address the channel guards">Start</Th>
                    <Th hint="Raw SM control byte: buffer mode, direction and watchdog">Control</Th>
                    <Th hint="Whether the master enables the channel">Enabled</Th>
                  </tr>
                </thead>
                <tbody>
                  {slave.processData.syncManagers.map(sm => (
                    <tr key={sm.index} className="border-b border-grey-100 last:border-0">
                      <td className="px-3 py-2 font-mono">SM{sm.index}</td>
                      <td className="px-3 py-2 text-grey-700">{sm.type}</td>
                      <td className="px-3 py-2 font-mono">{formatHex(sm.startAddress)}</td>
                      <td className="px-3 py-2 font-mono text-grey-500">
                        {formatHex(sm.controlByte, 2)}
                      </td>
                      <td className="px-3 py-2">{sm.enable ? 'yes' : 'no'}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </div>
      )}

      {slave.mailbox && (
        <div className="space-y-2">
          <p className="eyebrow text-grey-500">Mailbox</p>
          <dl className="grid grid-cols-2 sm:grid-cols-4 gap-4">
            <Field label="To device">
              {formatHex(slave.mailbox.send.start)} · {slave.mailbox.send.length} B
            </Field>
            <Field label="From device">
              {formatHex(slave.mailbox.recv.start)} · {slave.mailbox.recv.length} B
            </Field>
            <Field label="Protocols">{slave.mailbox.protocols.join(', ') || '—'}</Field>
            <Field label="Bootstrap">{slave.mailbox.bootstrap ? 'yes' : 'no'}</Field>
          </dl>
          {slave.mailbox.coeInitCmds.length > 0 && (
            <>
              <p className="text-xs text-grey-600">
                CoE transfers, which configure the device itself rather than its ESC — the PDO
                assignment lives here.
              </p>
              <CoeCmdTable cmds={slave.mailbox.coeInitCmds} />
            </>
          )}
        </div>
      )}

      {slave.dc && (
        <div className="space-y-2">
          <p className="eyebrow text-grey-500">Distributed clocks</p>
          <dl className="grid grid-cols-2 sm:grid-cols-4 gap-4">
            <Field label="Reference clock">{slave.dc.referenceClock ? 'yes' : 'no'}</Field>
            <Field label="Could be">{slave.dc.potentialReferenceClock ? 'yes' : 'no'}</Field>
            <Field label="SYNC0 cycle">
              {slave.dc.cycleTime0Ns !== undefined ? `${slave.dc.cycleTime0Ns} ns` : '—'}
            </Field>
            <Field label="SYNC0 shift">
              {slave.dc.shiftTimeNs !== undefined ? `${slave.dc.shiftTimeNs} ns` : '—'}
            </Field>
          </dl>
        </div>
      )}

      <div className="space-y-2">
        <p className="eyebrow text-grey-500">Init commands</p>
        <InitCmdTable cmds={slave.initCmds} />
      </div>
    </Section>
  )
}

export default function EniView({ parsed }: { parsed: EniParseResult }) {
  const { network, warnings, summary } = parsed
  return (
    <div className="space-y-4">
      {warnings.length > 0 && (
        <section className="border border-status-warn/40 bg-status-warn/10 px-4 py-3 space-y-1">
          <p className="eyebrow">
            {warnings.length} {warnings.length === 1 ? 'warning' : 'warnings'}
          </p>
          <ul className="text-xs text-grey-700 font-mono space-y-0.5">
            {warnings.map((warning, i) => (
              <li key={i}>{warning}</li>
            ))}
          </ul>
          <p className="text-xs text-grey-600">
            Each names something the document holds that this reader has no room for. Everything
            else was read.
          </p>
        </section>
      )}

      <p className="text-xs text-grey-600 font-mono">
        {summary.devices} {summary.devices === 1 ? 'device' : 'devices'} · {summary.datagrams}{' '}
        datagrams · {summary.coeTransfers} CoE transfers
      </p>

      <Section title="Master" subtitle={network.master.name} defaultOpen>
        <dl className="grid grid-cols-2 sm:grid-cols-4 gap-4">
          <Field label="Destination MAC">{network.master.destination}</Field>
          <Field label="Source MAC">{network.master.source}</Field>
          {network.master.mailboxStates && (
            <Field label="Mailbox check">
              {network.master.mailboxStates.count} devices @{' '}
              {formatHex(network.master.mailboxStates.startAddr, 8)}
            </Field>
          )}
          {network.master.eoe && (
            <Field label="EoE switch">
              {network.master.eoe.maxPorts} ports · {network.master.eoe.maxFrames} frames
            </Field>
          )}
        </dl>
        {network.master.initCmds.length > 0 && (
          <>
            <p className="text-xs text-grey-600">
              Bus-wide commands, sent before any device's own. These are the clears a device's
              commands assume have already run.
            </p>
            <InitCmdTable cmds={network.master.initCmds} />
          </>
        )}
      </Section>

      {network.slaves.map((slave, i) => (
        <SlaveCard key={i} slave={slave} position={i + 1} />
      ))}

      {network.cyclic && (
        <Section
          title="Cyclic"
          subtitle={
            network.cyclic.cycleTimeUs !== undefined
              ? `every ${network.cyclic.cycleTimeUs} µs`
              : undefined
          }
        >
          {network.cyclic.frames.map((frame, i) => (
            <div key={i} className="space-y-2">
              {frame.comment && <p className="text-xs text-grey-600">{frame.comment}</p>}
              <InitCmdTable cmds={frame.cmds} />
            </div>
          ))}
        </Section>
      )}

      {network.processImage && (
        <Section title="Process image">
          {(['outputs', 'inputs'] as const).map(half => {
            const area = network.processImage?.[half]
            if (!area) {
              return null
            }
            return (
              <div key={half} className="space-y-2">
                <p className="eyebrow text-grey-500">
                  {half} · {area.byteSize} B
                </p>
                {area.variables.length === 0 ? (
                  <p className="text-xs text-grey-500">No named variables.</p>
                ) : (
                  <div className="border border-grey-200 overflow-x-auto">
                    <table className="w-full min-w-[560px] text-xs border-collapse">
                      <thead>
                        <tr className="border-b border-grey-200 bg-grey-50 text-left text-grey-600">
                          <Th hint="Which device's window this value lands in, worked out from the offset rather than from the name">
                            Device
                          </Th>
                          <Th hint="The scope the writing tool prefixed to the name, before the first dot">
                            Scope
                          </Th>
                          <Th hint="The signal itself, after the scope prefix">Name</Th>
                          <Th hint="Type name spelled the way an ESI spells it">Type</Th>
                          <Th hint="Width in bits">Size</Th>
                          <Th hint="Offset within this half of the image, in bits">Offset</Th>
                        </tr>
                      </thead>
                      <tbody>
                        {area.variables.map((variable, j) => {
                          const { scope, signal } = splitVariableName(variable.name)
                          const device = deviceAt(network.slaves, half, variable.bitOffs)
                          return (
                            <tr key={j} className="border-b border-grey-100 last:border-0">
                              <td className="px-3 py-2 font-mono">
                                {device !== null ? `#${device}` : <span className="text-grey-400">—</span>}
                              </td>
                              <td className="px-3 py-2 text-grey-600">
                                {scope ?? <span className="text-grey-400">—</span>}
                              </td>
                              <td className="px-3 py-2">{signal}</td>
                              <td className="px-3 py-2 font-mono text-grey-600">
                                {variable.dataType ?? '—'}
                              </td>
                              <td className="px-3 py-2 font-mono">{variable.bitSize}</td>
                              <td className="px-3 py-2 font-mono text-grey-500">
                                {variable.bitOffs}
                              </td>
                            </tr>
                          )
                        })}
                      </tbody>
                    </table>
                  </div>
                )}
              </div>
            )
          })}
        </Section>
      )}
    </div>
  )
}
