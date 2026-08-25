import { useMutation, useQueryClient } from '@tanstack/react-query'
import type { FsoeConnection } from '@synapticon/motion-master-client'
import Section from './Section'
import { btnCls, btnGhostCls } from './controlStyles'
import { useConnection } from '../contexts/ConnectionContext'

/**
 * The safety controlword, one row per function.
 *
 * Each stop function gets its OWN pair of controls, because they are separate
 * bits and the bench needs them separately: releasing STO must not release SS1,
 * or an axis can never be run up and then stopped with SS1 - which is the one
 * thing this page exists to let somebody do.
 *
 * The bits are inverted on the wire (ETG.6100.2 Table 3): a CLEAR bit is a
 * request, so an all-zero frame asks for every stop function at once. That is
 * the right failure direction and it is also the trap - a controlword of 0x00
 * with STO released still reads 0x01, which leaves SS1 requested and the axis
 * held. The decode at the foot of this panel is here so that state is legible
 * rather than deduced.
 */

const STO_BIT = 0
const SS1_BIT = 1
const ERROR_ACK_BIT = 7

/** How long the error-acknowledge bit is held high. The drive acts on the rising edge, and one
 *  exchange is ~3 bus cycles, so this is many exchanges wide rather than a guess at one. */
const ACK_PULSE_MS = 150

function bitSet(octet: number, bit: number): boolean {
  return (octet & (1 << bit)) !== 0
}

function Chip({
  tone,
  children,
  hint,
}: {
  tone: 'ok' | 'warn' | 'bad' | 'info'
  children: React.ReactNode
  hint?: string
}) {
  const cls = {
    ok: 'bg-status-good/10 text-status-good border-status-good/30',
    warn: 'bg-status-warn/10 text-status-warn border-status-warn/40',
    bad: 'bg-status-bad/10 text-status-bad border-status-bad/40',
    info: 'bg-grey-100 text-grey-600 border-grey-300',
  }[tone]
  return (
    <span
      title={hint}
      className={`inline-flex items-center h-[18px] px-1.5 text-[10px] tracking-wide border ${cls} ${
        hint ? 'cursor-help' : ''
      }`}
    >
      {children}
    </span>
  )
}

/**
 * One safety function: what it is, what the drive says about it, and its controls.
 *
 * `status` is deliberately separate from the buttons' state. What was asked for and what the
 * drive reports are different facts, and a panel that showed only the request would say "released"
 * about an axis the drive is still holding.
 */
function FunctionRow({
  name,
  reference,
  status,
  note,
  release,
  activate,
}: {
  name: string
  reference: string
  status: React.ReactNode
  note?: React.ReactNode
  release: React.ReactNode
  activate: React.ReactNode
}) {
  return (
    <div className="px-4 py-3 border-b border-grey-200 last:border-b-0">
      <div className="flex items-center gap-3 flex-wrap">
        <div className="min-w-[9rem]">
          <div className="text-sm text-grey-900">{name}</div>
          <div className="text-[10px] text-grey-500">{reference}</div>
        </div>
        {status}
        {/* Two fixed slots rather than a free row of buttons: the same column means the same kind
            of action on every row, so "the left one lets the axis move, the right one stops it" is
            learnable once instead of read per row. An empty slot keeps the alignment. */}
        <div className="ml-auto flex items-center gap-2">
          <div className="w-[8.5rem] flex justify-end">{release}</div>
          <div className="w-[8.5rem] flex justify-end">{activate}</div>
        </div>
      </div>
      {note && <p className="mt-2 text-xs text-grey-600 max-w-3xl">{note}</p>}
    </div>
  )
}

export default function SafetyControlPanel({
  slavePosition,
  connection,
  onMessage,
  onError,
}: {
  slavePosition: number
  connection: FsoeConnection
  onMessage: (text: string) => void
  onError: (err: unknown) => void
}) {
  const { api } = useConnection()
  const queryClient = useQueryClient()
  const invalidate = () => queryClient.invalidateQueries({ queryKey: ['fsoe'] })

  const controlword = connection.safeOutputs?.[0] ?? 0
  const statusword = connection.safeInputs?.[0] ?? 0

  // What was ASKED for, read back off the wire rather than kept in component state, so the panel
  // cannot drift out of step with the frame actually being sent.
  const stoReleased = bitSet(controlword, STO_BIT)
  const ss1Released = bitSet(controlword, SS1_BIT)

  // What the DRIVE reports. There is no SS1 bit in the safety statusword - Table 4 gives bit 1 to
  // SSM_1 - so a completed SS1 shows up as STO active, not as an SS1 flag.
  const stoActive = connection.safetyStatus?.stoActive ?? true
  const driveError = connection.safetyStatus?.error ?? false

  const inData = connection.state === 'Data'
  const sendingProcessData = connection.dataCommand === 'ProcessData'

  /* One rule, both functions: RELEASING needs a live process-data link, because a release that the
     link is not carrying would report a permission the drive never got. ACTIVATING never needs it -
     it is the safe direction, and refusing to make an axis safer because of the transport would be
     the wrong way round. */
  const releasable = inData && sendingProcessData

  const setSto = useMutation({
    mutationFn: (released: boolean) => api.setFsoeSto(slavePosition, { released }),
    onSuccess: (_d, released) => {
      onMessage(
        released
          ? 'STO released. Torque is permitted once SS1 is released too.'
          : 'STO applied: torque removed.',
      )
      invalidate()
    },
    onError,
  })

  const setSs1 = useMutation({
    mutationFn: (requested: boolean) => api.setFsoeSs1(slavePosition, { requested }),
    onSuccess: (_d, requested) => {
      onMessage(
        requested
          ? 'SS1 requested. The drive brings the axis down and then removes torque.'
          : 'SS1 request released. A stop already running still finalizes.',
      )
      invalidate()
    },
    onError,
  })

  /* Acknowledge is an edge, not a level, so it is a pulse rather than a toggle: raise bit 7, hold
     it across several exchanges, lower it. Written through the raw SafeOutputs endpoint because it
     is the only one that can set bit 7 - and read-modify-write off the current octets, so the pulse
     cannot disturb the stop-function bits somebody has already set. */
  const acknowledge = useMutation({
    mutationFn: async () => {
      const base = [...(connection.safeOutputs ?? [])]
      if (base.length === 0) throw new Error('this connection carries no SafeOutputs')
      const raised = [...base]
      raised[0] = base[0] | (1 << ERROR_ACK_BIT)
      await api.setFsoeSafeOutputs(slavePosition, { data: raised })
      await new Promise(resolve => setTimeout(resolve, ACK_PULSE_MS))
      await api.setFsoeSafeOutputs(slavePosition, { data: base })
    },
    onSuccess: () => {
      onMessage('Error acknowledged.')
      invalidate()
    },
    onError,
  })

  const busy = setSto.isPending || setSs1.isPending || acknowledge.isPending

  // The summary that answers the only question a bench operator actually has. Torque needs all
  // four, and naming the one that fails beats a disabled button with no explanation.
  const blockers: string[] = []
  if (!inData) blockers.push(`the connection is in ${connection.state}, not Data`)
  if (!sendingProcessData) blockers.push('the master is sending FailSafeData')
  if (!stoReleased) blockers.push('STO is requested')
  if (!ss1Released) blockers.push('SS1 is requested')

  return (
    <Section
      title="Safety control"
      chips={
        <>
          {blockers.length === 0 && !stoActive ? (
            <Chip tone="bad" hint="Every condition for torque is met and the drive confirms STO is released.">
              torque permitted
            </Chip>
          ) : (
            <Chip tone="ok" hint="The drive is holding the axis. Nothing here can produce torque.">
              axis held
            </Chip>
          )}
          {driveError && (
            <Chip tone="bad" hint="Safety statusword bit 7. Acknowledge it before the drive will run again.">
              drive error
            </Chip>
          )}
        </>
      }
      description={
        <>
          Each stop function is its own bit and its own pair of buttons: <strong>release</strong> on
          the left permits motion, <strong>activate</strong> on the right removes it. Releasing STO
          does not release SS1 — the wire inverts both, so an all-zero controlword requests every
          stop at once, and permitting motion means releasing each function you are not testing.
        </>
      }
    >
      <FunctionRow
        name="Safe Torque Off"
        reference="controlword bit 0"
        status={
          <Chip
            tone={stoActive ? 'ok' : 'bad'}
            hint="What the drive reports in its safety statusword, not what was asked for."
          >
            {stoActive ? 'STO active — no torque' : 'STO released — torque permitted'}
          </Chip>
        }
        release={
          <button
            type="button"
            className={btnGhostCls + ' w-full'}
            title="Deactivate STO: permit torque. Bit 0 goes high."
            disabled={busy || !releasable || stoReleased}
            onClick={() => setSto.mutate(true)}
          >
            Release
          </button>
        }
        activate={
          <button
            type="button"
            className={btnCls + ' w-full'}
            title="Activate STO: remove torque. Bit 0 goes low."
            disabled={busy || !stoReleased}
            onClick={() => setSto.mutate(false)}
          >
            Activate
          </button>
        }
      />

      <FunctionRow
        name="Safe Stop 1"
        reference="controlword bit 1"
        status={
          <Chip
            tone={ss1Released ? 'info' : 'warn'}
            hint="What this master is requesting. The safety statusword has no SS1 bit — Table 4 gives bit 1 to SSM_1 — so a finished stop shows up as STO active."
          >
            {ss1Released ? 'not requested' : 'SS1 requested'}
          </Chip>
        }
        note={
          !ss1Released
            ? 'Releasing the request does not abort the stop: ETG.6100.2 ch. 8.2.1.1 requires an activated SS1 to be finalized. Release it to let the axis run again once the stop has ended.'
            : undefined
        }
        release={
          <button
            type="button"
            className={btnGhostCls + ' w-full'}
            title="Deactivate SS1: stop requesting a stop. Bit 1 goes high. It does not abort a stop already running."
            disabled={busy || !releasable || ss1Released}
            onClick={() => setSs1.mutate(false)}
          >
            Release
          </button>
        }
        activate={
          <button
            type="button"
            className={btnCls + ' w-full'}
            title="Activate SS1: bring the axis down, then remove torque. Bit 1 goes low."
            disabled={busy || !ss1Released}
            onClick={() => setSs1.mutate(true)}
          >
            Activate
          </button>
        }
      />

      <FunctionRow
        name="Error acknowledge"
        reference="controlword bit 7"
        status={
          <Chip tone={driveError ? 'bad' : 'info'}>
            {driveError ? 'error latched' : 'no error'}
          </Chip>
        }
        release={
          <button
            type="button"
            className={btnGhostCls + ' w-full'}
            title="Pulse bit 7. The drive acts on the rising edge, so this raises it, holds it for several exchanges and lowers it again."
            disabled={busy || !releasable}
            onClick={() => acknowledge.mutate()}
          >
            {acknowledge.isPending ? 'Acknowledging' : 'Acknowledge'}
          </button>
        }
        activate={null}
      />

      <div className="px-4 py-3 bg-grey-50 border-t border-grey-200 text-xs">
        {blockers.length > 0 ? (
          <p className="text-grey-700">
            <span className="text-grey-500">Torque is not possible: </span>
            {blockers.join('; ')}.
          </p>
        ) : (
          <p className="text-grey-700">
            <span className="text-grey-500">All four conditions for torque are met: </span>
            connection in Data, sending ProcessData, STO released, SS1 released.
          </p>
        )}
        <div className="mt-2 flex flex-wrap gap-x-6 gap-y-1 font-mono text-[11px] text-grey-600">
          <span>
            controlword 0x{controlword.toString(16).toUpperCase().padStart(2, '0')} · STO=
            {stoReleased ? 1 : 0} SS1={ss1Released ? 1 : 0} ack=
            {bitSet(controlword, ERROR_ACK_BIT) ? 1 : 0} <span className="text-grey-400">(1 = released)</span>
          </span>
          <span>
            statusword 0x{statusword.toString(16).toUpperCase().padStart(2, '0')} · STO active=
            {stoActive ? 1 : 0} error={driveError ? 1 : 0}
          </span>
        </div>
      </div>
    </Section>
  )
}
