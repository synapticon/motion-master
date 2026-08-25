import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useConnection } from '../contexts/ConnectionContext'

/* AL states that have a working CoE mailbox: PRE-OP, SAFE-OP, OP. INIT has no mailbox at all and
   BOOT's speaks FoE, not CoE, so the SDO fallback below must not run there. */
const MAILBOX_ACTIVE_STATES = new Set([2, 4, 8])

/**
 * The safe sensor's configuration and diagnosis, objects 0x2601/0x2602/0x2603/0x2605.
 *
 * These read over plain SDO, not through the FSoE connection, and that is
 * deliberate: a wrong safety address is exactly why a connection will not open,
 * and the diagnosis is most wanted precisely then. The panel therefore works
 * with no connection at all.
 */

// One increment of the safe position is 2^-24 of a revolution (object 0x6601
// declares the unit; the count spans the whole multiturn range).
const INCREMENTS_PER_REV = 1 << 24

const OBJ = {
  posTolerance: [0x2601, 1], posNow: [0x2601, 2], posMaxPos: [0x2601, 3], posMaxNeg: [0x2601, 4],
  posReason: [0x2601, 5], posCauses: [0x2601, 6], posInvalidations: [0x2601, 7],
  velFilterMs: [0x2602, 1], velTolerance: [0x2602, 2], velFilterUs: [0x2602, 3],
  velNow: [0x2602, 4], velMaxPos: [0x2602, 5], velMaxNeg: [0x2602, 6],
  velReason: [0x2602, 7], velCauses: [0x2602, 8], velInvalidations: [0x2602, 9],
  trqFilterMs: [0x2603, 1], trqSumTol: [0x2603, 2], trqFilterUs: [0x2603, 3],
  trqCrossTol: [0x2603, 4], trqReason: [0x2603, 5], trqCauses: [0x2603, 6],
  trqInvalidations: [0x2603, 7],
  debounceMs: [0x2605, 1], xcReason: [0x2605, 2], xcCauses: [0x2605, 3],
  cyclePeriodUs: [0x2605, 4], sampleTimeoutUs: [0x2605, 5],
  paramState: [0x2605, 6], paramCrc: [0x2605, 7],
  faultSources: [0x2605, 8], leaseError: [0x2605, 9],
} as const

type ObjKey = keyof typeof OBJ

// SafeSensorInvalidReason. The three "not a fault" cross-check states are
// styled as information rather than as problems, because a single-encoder axis
// reports one of them for the whole of its life and it is not broken.
const REASON: Record<number, { label: string; tone: 'ok' | 'info' | 'warn' | 'bad'; hint: string }> = {
  0:  { label: 'Ok', tone: 'ok', hint: 'The value is valid.' },
  1:  { label: 'No sample', tone: 'info', hint: 'The pipeline has taken no coherent sample yet. With no safety master connected it is dormant, which is the resting state rather than a fault.' },
  2:  { label: 'Stale', tone: 'bad', hint: 'Acquisition stopped presenting new samples past the sample hold timeout.' },
  3:  { label: 'Sensor fault', tone: 'bad', hint: 'The acquisition flagged the reading itself as bad.' },
  4:  { label: 'Window filling', tone: 'warn', hint: 'An averaging window has not filled. The average would read low, which understates speed or torque - the dangerous direction - so the value is withheld until it has.' },
  5:  { label: 'Position discrepancy', tone: 'bad', hint: 'The channels’ positions differ by more than the allowed discrepancy, for longer than the debounce.' },
  6:  { label: 'Implausible currents', tone: 'bad', hint: 'The three phase currents do not sum to zero within tolerance.' },
  7:  { label: 'Unconfigured', tone: 'bad', hint: 'The configuration was rejected; no sample can change that until it is fixed.' },
  8:  { label: 'Parameters unvalidated', tone: 'warn', hint: 'The safety parameters have not been agreed with the master, so the scaling is unagreed.' },
  9:  { label: 'Bad primary encoder', tone: 'bad', hint: 'Primary resolution, or the wrap derived from it, is out of range.' },
  10: { label: 'Bad verification encoder', tone: 'bad', hint: 'Verification resolution, or the wrap derived from it, is out of range.' },
  11: { label: 'Bad gear ratio', tone: 'bad', hint: 'A gear term is larger than the pipeline can carry.' },
  12: { label: 'No cycle period', tone: 'bad', hint: 'The safety cycle period is zero, so the filter windows have no length.' },
  13: { label: 'Bad torque scaling', tone: 'bad', hint: 'Torque constant or current ratio is not positive.' },
  14: { label: 'Scaling overflow', tone: 'bad', hint: 'The reduced position scaling does not fit the arithmetic budget.' },
  15: { label: 'Invalidated', tone: 'bad', hint: 'Something invalidated the pipeline; the absolute reference is latched lost.' },
  16: { label: 'Inbox torn', tone: 'bad', hint: 'The sampler is publishing faster than the pipeline can read a coherent snapshot.' },
  17: { label: 'Primary count out of range', tone: 'bad', hint: 'The raw count lies outside the configured resolution - a commissioning error, not a sick sensor.' },
  18: { label: 'Verification count out of range', tone: 'bad', hint: 'As above, for the verification channel.' },
  19: { label: 'Velocity discrepancy', tone: 'bad', hint: 'The channels’ velocities differ by more than the allowed discrepancy.' },
  20: { label: 'Verification channel down', tone: 'bad', hint: 'The verification channel is not delivering, so no comparison can pass. Widening a tolerance will not help.' },
  21: { label: 'Current out of range', tone: 'bad', hint: 'A phase magnitude exceeds what the acquisition can represent.' },
  22: { label: 'No verification channel', tone: 'info', hint: 'One encoder is fitted, so there is nothing to cross-check. Not a fault - this is the correct report for a single-channel axis.' },
  23: { label: 'Cross-check disabled', tone: 'warn', hint: 'Two channels are fitted and both tolerances are zero, so nothing is being compared. Almost always a commissioning omission.' },
  24: { label: 'Reference not captured', tone: 'info', hint: 'The alignment offset between the channels has never been taken.' },
  25: { label: 'Reference lost', tone: 'bad', hint: 'The reference was lost and no later sample can recover it.' },
  26: { label: 'Velocity saturated', tone: 'warn', hint: 'The velocity clipped its numeric rail. The value stays valid.' },
  27: { label: 'Torque saturated', tone: 'warn', hint: 'The torque clipped its numeric rail. The value stays valid.' },
  28: { label: 'Position rollover', tone: 'info', hint: 'The position accumulator wrapped its multiturn range. Expected on a continuously rotating axis.' },
}

const TONE_CLS = {
  ok:   'bg-status-good text-white',
  info: 'bg-grey-200 text-grey-700',
  warn: 'bg-status-warn text-grey-900',
  bad:  'bg-status-bad text-white',
}

const FAULT_SOURCE = [
  'STO diagnostic', 'STO read-back mismatch', 'Execution supervision', 'Sensor', 'SafeInputs too short',
]

/** Entries whose wire type is signed, so an SDO upload must sign-extend. */
const SIGNED = new Set<ObjKey>([
  'posNow', 'posMaxPos', 'posMaxNeg', 'velNow', 'velMaxPos', 'velMaxNeg',
])

/** Little-endian bytes from an SDO upload to a number. */
function decodeLE(bytes: number[], signed: boolean): number {
  let v = 0
  for (let i = bytes.length - 1; i >= 0; i--) v = v * 256 + bytes[i]
  if (signed && bytes.length === 4 && v > 0x7fffffff) v -= 0x100000000
  if (signed && bytes.length === 2 && v > 0x7fff) v -= 0x10000
  return v
}

function ReasonChip({ code }: { code: number | undefined }) {
  if (code === undefined) return null
  const r = REASON[code] ?? { label: `Reason ${code}`, tone: 'warn' as const, hint: 'Unknown reason code.' }
  return (
    <span title={r.hint} className={`inline-flex items-center h-[18px] px-1.5 text-[10px] tracking-wide cursor-help ${TONE_CLS[r.tone]}`}>
      {r.label}
    </span>
  )
}

/** Every reason ever seen since this connection came up, as chips. */
function CauseChips({ mask }: { mask: number | undefined }) {
  if (!mask) return null
  const seen = Object.keys(REASON)
    .map(Number)
    .filter(c => c !== 0 && (mask & (1 << c)) !== 0)
  if (seen.length === 0) return null
  return (
    <div className="mt-2 flex flex-wrap gap-1">
      <span className="text-[10px] uppercase tracking-wider text-grey-500 mr-1">seen</span>
      {seen.map(c => (
        <span key={c} title={REASON[c].hint}
              className="inline-flex items-center h-[16px] px-1 text-[9px] border border-grey-300 text-grey-600 cursor-help">
          {REASON[c].label}
        </span>
      ))}
    </div>
  )
}

/**
 * The headroom bar: where the channels have actually been, against the tolerance
 * that would trip them.
 *
 * This is the instrument for choosing a tolerance. The band is +/- the configured
 * tolerance; the grey span is the range the channels have covered since the
 * connection came up, and the needle is where they are now. If the span nearly
 * fills the band the axis is about to nuisance-trip; if it occupies a sliver,
 * the tolerance is far looser than the machine needs.
 */
function HeadroomBar({ now, maxPos, maxNeg, tolerance, format }: {
  now: number; maxPos: number; maxNeg: number; tolerance: number
  format: (v: number) => string
}) {
  if (!tolerance) {
    return <div className="mt-2 text-[10px] text-grey-500">No tolerance set, so nothing is being compared.</div>
  }
  const pct = (v: number) => 50 + Math.max(-50, Math.min(50, (v / tolerance) * 50))
  const left = pct(Math.min(maxNeg, 0))
  const right = pct(Math.max(maxPos, 0))
  const worst = Math.max(Math.abs(maxPos), Math.abs(maxNeg))
  const used = tolerance ? worst / tolerance : 0
  const tone = used > 0.9 ? 'bg-status-bad' : used > 0.6 ? 'bg-status-warn' : 'bg-status-good'
  return (
    <div className="mt-3">
      <div className="relative h-[22px] border border-grey-300 bg-grey-50">
        {/* the observed span */}
        <div className={`absolute top-0 bottom-0 ${tone} opacity-40`}
             style={{ left: `${left}%`, width: `${Math.max(0.8, right - left)}%` }} />
        {/* centre line: perfect agreement */}
        <div className="absolute top-0 bottom-0 w-px bg-grey-400" style={{ left: '50%' }} />
        {/* live needle */}
        <div className="absolute top-0 bottom-0 w-[2px] bg-grey-900" style={{ left: `${pct(now)}%` }}
             title={`now ${format(now)}`} />
      </div>
      <div className="mt-1 flex justify-between text-[9px] text-grey-500 font-mono">
        <span>-{format(tolerance)}</span>
        <span className="text-grey-700">{format(now)} now</span>
        <span>+{format(tolerance)}</span>
      </div>
      <div className="text-[10px] text-grey-500">
        worst seen <span className="font-mono text-grey-700">{format(maxNeg)}</span> to{' '}
        <span className="font-mono text-grey-700">{format(maxPos)}</span>
        {' '}&mdash; using <span className="font-mono text-grey-700">{(used * 100).toFixed(0)}%</span> of the tolerance
      </div>
    </div>
  )
}

export default function SafeSensorPanel({ slavePosition }: { slavePosition: number }) {
  const { api } = useConnection()
  const queryClient = useQueryClient()
  const [edits, setEdits] = useState<Partial<Record<ObjKey, string>>>({})
  const [error, setError] = useState<string | null>(null)

  const statesQuery = useQuery({
    queryKey: ['deviceStates'],
    queryFn: () => api.getDeviceStates(),
  })
  const alState = statesQuery.data?.data.find((s) => s.slavePosition === slavePosition)?.alState
  const mailboxActive = alState !== undefined && MAILBOX_ACTIVE_STATES.has(alState)

  const q = useQuery({
    queryKey: ['safeSensor', slavePosition],
    queryFn: async () => {
      const out: Partial<Record<ObjKey, number>> = {}
      for (const [key, [index, sub]] of Object.entries(OBJ)) {
        const signed = SIGNED.has(key as ObjKey)
        let value: number | undefined
        try {
          const res = await api.readParameter(slavePosition, index, sub)
          value = Number(res.data.value)
        } catch {
          /* readParameter answers from the enumerated object dictionary, which
             is empty until a device has been enumerated - and after a firmware
             update it is empty exactly when someone most wants to look. An SDO
             upload asks the device itself and always works, so it is the
             fallback rather than the last resort. */
          try {
            const raw = await api.sdoUpload(slavePosition, index, sub)
            value = decodeLE(raw.data.data, signed)
          } catch {
            /* A device without the safe-sensor option answers some of these and
               not others; a missing entry is left undefined rather than zero, so
               the panel can say "not present" instead of inventing a reading. */
          }
        }
        if (value !== undefined) out[key as ObjKey] = value
      }
      return out
    },
    enabled: mailboxActive,
    refetchInterval: 1000,
  })

  const write = useMutation({
    mutationFn: async ({ key, value }: { key: ObjKey; value: number }) => {
      const [index, sub] = OBJ[key]
      await api.writeParameter(slavePosition, index, sub, { value })
    },
    onSuccess: () => { setError(null); queryClient.invalidateQueries({ queryKey: ['safeSensor', slavePosition] }) },
    onError: (e: unknown) => setError(String(e)),
  })

  const d = q.data ?? {}
  const present = d.cyclePeriodUs !== undefined
  const revs = (inc: number) => `${(inc / INCREMENTS_PER_REV).toFixed(4)} rev`
  const mrpm = (v: number) => `${v} mRPM`

  if (q.isLoading) {
    return <div className="border border-grey-200 p-4 text-sm text-grey-500">Reading the safe sensor objects…</div>
  }
  if (!present) {
    return (
      <div className="border border-grey-200 p-4 text-sm text-grey-500">
        This device does not answer object 0x2605, so it carries no safe-sensor option.
      </div>
    )
  }

  const numEdit = (key: ObjKey) => edits[key] ?? String(d[key] ?? '')
  const commit = (key: ObjKey) => {
    const v = Number(edits[key])
    if (!Number.isFinite(v)) return
    write.mutate({ key, value: Math.round(v) })
    setEdits(e => ({ ...e, [key]: undefined }))
  }

  /** Turn the worst disagreement actually observed into a tolerance, doubled so
      a machine that behaves as it did today has margin rather than sitting on
      the edge of its own trip point. */
  const suggest = (maxPos = 0, maxNeg = 0) => Math.max(1, Math.ceil(Math.max(Math.abs(maxPos), Math.abs(maxNeg)) * 2))

  const Tol = ({ tolKey, maxPos, maxNeg, unit }: { tolKey: ObjKey; maxPos?: number; maxNeg?: number; unit: string }) => (
    <div className="mt-3 flex items-end gap-2">
      <label className="flex-1">
        <span className="block text-[10px] uppercase tracking-wider text-grey-500">Allowed discrepancy ({unit})</span>
        <input
          className="border border-grey-300 px-2 h-[30px] text-xs w-full bg-white font-mono"
          value={numEdit(tolKey)}
          onChange={e => setEdits(x => ({ ...x, [tolKey]: e.target.value }))}
          onKeyDown={e => { if (e.key === 'Enter') commit(tolKey) }}
        />
      </label>
      <button className="border border-grey-300 text-grey-700 px-2 h-[30px] text-[11px] hover:bg-grey-50 cursor-pointer"
              disabled={write.isPending}
              onClick={() => commit(tolKey)}>Set</button>
      <button
        className="border border-grey-300 text-grey-700 px-2 h-[30px] text-[11px] hover:bg-grey-50 cursor-pointer whitespace-nowrap"
        title="Take the worst disagreement seen since this connection came up, double it for margin, and use that."
        disabled={write.isPending}
        onClick={() => setEdits(x => ({ ...x, [tolKey]: String(suggest(maxPos, maxNeg)) }))}
      >
        from measured
      </button>
    </div>
  )

  return (
    <section className="mt-8">
      <div className="flex items-baseline justify-between">
        <h2 className="text-sm uppercase tracking-wider text-grey-500">Safe sensor</h2>
        <span className="text-[10px] text-grey-400">
          0x2601 / 0x2602 / 0x2603 / 0x2605 &middot; read over SDO, so this works with no FSoE connection
        </span>
      </div>

      {error && (
        <div className="mt-2 border border-status-bad px-3 py-2 text-xs text-status-bad">{error}</div>
      )}

      <div className="mt-3 grid gap-3 md:grid-cols-3">
        {/* ---- position ---- */}
        <div className="border border-grey-200 px-4 py-3">
          <div className="flex items-center justify-between gap-2">
            <span className="text-[10px] uppercase tracking-wider text-grey-500">Position</span>
            <ReasonChip code={d.posReason} />
          </div>
          <HeadroomBar now={d.posNow ?? 0} maxPos={d.posMaxPos ?? 0} maxNeg={d.posMaxNeg ?? 0}
                       tolerance={d.posTolerance ?? 0} format={revs} />
          <Tol tolKey="posTolerance" maxPos={d.posMaxPos} maxNeg={d.posMaxNeg} unit="increments" />
          <div className="mt-2 text-[10px] text-grey-500">
            dropped out <span className="font-mono text-grey-700">{d.posInvalidations ?? 0}</span> times
          </div>
          <CauseChips mask={d.posCauses} />
        </div>

        {/* ---- velocity ---- */}
        <div className="border border-grey-200 px-4 py-3">
          <div className="flex items-center justify-between gap-2">
            <span className="text-[10px] uppercase tracking-wider text-grey-500">Velocity</span>
            <ReasonChip code={d.velReason} />
          </div>
          <HeadroomBar now={d.velNow ?? 0} maxPos={d.velMaxPos ?? 0} maxNeg={d.velMaxNeg ?? 0}
                       tolerance={d.velTolerance ?? 0} format={mrpm} />
          <Tol tolKey="velTolerance" maxPos={d.velMaxPos} maxNeg={d.velMaxNeg} unit="mRPM" />
          <div className="mt-2 flex items-end gap-2">
            <label className="flex-1">
              <span className="block text-[10px] uppercase tracking-wider text-grey-500">Filter window (ms)</span>
              <input className="border border-grey-300 px-2 h-[30px] text-xs w-full bg-white font-mono"
                     value={numEdit('velFilterMs')}
                     onChange={e => setEdits(x => ({ ...x, velFilterMs: e.target.value }))}
                     onKeyDown={e => { if (e.key === 'Enter') commit('velFilterMs') }} />
            </label>
            <button className="border border-grey-300 text-grey-700 px-2 h-[30px] text-[11px] hover:bg-grey-50 cursor-pointer"
                    onClick={() => commit('velFilterMs')}>Set</button>
          </div>
          <div className="mt-1 text-[10px] text-grey-500">
            in force <span className="font-mono text-grey-700">{((d.velFilterUs ?? 0) / 1000).toFixed(1)} ms</span>
            {d.velFilterUs !== undefined && d.velFilterMs !== undefined &&
              d.velFilterUs !== d.velFilterMs * 1000 && (
              <span className="ml-1 text-status-warn" title="The request was longer than the averaging ring can hold, so it was truncated.">
                (truncated)
              </span>
            )}
          </div>
          <div className="mt-1 text-[10px] text-grey-500">
            dropped out <span className="font-mono text-grey-700">{d.velInvalidations ?? 0}</span> times
          </div>
          <CauseChips mask={d.velCauses} />
        </div>

        {/* ---- torque ---- */}
        <div className="border border-grey-200 px-4 py-3">
          <div className="flex items-center justify-between gap-2">
            <span className="text-[10px] uppercase tracking-wider text-grey-500">Torque</span>
            <ReasonChip code={d.trqReason} />
          </div>
          <div className="mt-3 text-[10px] text-grey-500 leading-relaxed">
            Torque is derived from phase currents on one acquisition path, so there is no second
            channel to compare against. The check available is that the three phases sum to zero -
            which is blind to a reference or gain error, because that scales all three together.
          </div>
          <div className="mt-3 flex items-end gap-2">
            <label className="flex-1">
              <span className="block text-[10px] uppercase tracking-wider text-grey-500">Current sum tolerance (ADC)</span>
              <input className="border border-grey-300 px-2 h-[30px] text-xs w-full bg-white font-mono"
                     value={numEdit('trqSumTol')}
                     onChange={e => setEdits(x => ({ ...x, trqSumTol: e.target.value }))}
                     onKeyDown={e => { if (e.key === 'Enter') commit('trqSumTol') }} />
            </label>
            <button className="border border-grey-300 text-grey-700 px-2 h-[30px] text-[11px] hover:bg-grey-50 cursor-pointer"
                    onClick={() => commit('trqSumTol')}>Set</button>
          </div>
          <div className="mt-2 flex items-end gap-2">
            <label className="flex-1">
              <span className="block text-[10px] uppercase tracking-wider text-grey-500">Filter time (ms)</span>
              <input className="border border-grey-300 px-2 h-[30px] text-xs w-full bg-white font-mono"
                     value={numEdit('trqFilterMs')}
                     onChange={e => setEdits(x => ({ ...x, trqFilterMs: e.target.value }))}
                     onKeyDown={e => { if (e.key === 'Enter') commit('trqFilterMs') }} />
            </label>
            <button className="border border-grey-300 text-grey-700 px-2 h-[30px] text-[11px] hover:bg-grey-50 cursor-pointer"
                    onClick={() => commit('trqFilterMs')}>Set</button>
          </div>
          <div className="mt-1 text-[10px] text-grey-500">
            in force <span className="font-mono text-grey-700">{((d.trqFilterUs ?? 0) / 1000).toFixed(1)} ms</span>
            <span className="ml-1" title="Any averaging attenuates a peak, and the safe torque is published as an upper bound, so a longer filter under-reports a transient.">&#9432;</span>
          </div>
          <div className="mt-1 text-[10px] text-grey-500">
            dropped out <span className="font-mono text-grey-700">{d.trqInvalidations ?? 0}</span> times
          </div>
          <CauseChips mask={d.trqCauses} />
        </div>
      </div>

      {/* ---- cross-check and the pipeline-wide state ---- */}
      <div className="mt-3 border border-grey-200 px-4 py-3">
        <div className="grid gap-4 md:grid-cols-4">
          <div>
            <div className="text-[10px] uppercase tracking-wider text-grey-500">Cross-check</div>
            <div className="mt-1"><ReasonChip code={d.xcReason} /></div>
            <CauseChips mask={d.xcCauses} />
          </div>
          <div>
            <div className="text-[10px] uppercase tracking-wider text-grey-500">Discrepancy debounce</div>
            <div className="mt-1 flex items-end gap-2">
              <input className="border border-grey-300 px-2 h-[30px] text-xs w-24 bg-white font-mono"
                     value={numEdit('debounceMs')}
                     onChange={e => setEdits(x => ({ ...x, debounceMs: e.target.value }))}
                     onKeyDown={e => { if (e.key === 'Enter') commit('debounceMs') }} />
              <button className="border border-grey-300 text-grey-700 px-2 h-[30px] text-[11px] hover:bg-grey-50 cursor-pointer"
                      onClick={() => commit('debounceMs')}>Set</button>
            </div>
            <div className="mt-1 text-[10px] text-grey-500">ms the channels may disagree before it counts</div>
          </div>
          <div>
            <div className="text-[10px] uppercase tracking-wider text-grey-500">Timing</div>
            <div className="mt-1 text-xs font-mono text-grey-700">
              {(d.cyclePeriodUs ?? 0)} µs cycle
            </div>
            <div className="text-xs font-mono text-grey-700">{(d.sampleTimeoutUs ?? 0)} µs sample hold</div>
          </div>
          <div>
            <div className="text-[10px] uppercase tracking-wider text-grey-500">Safety parameters</div>
            <div className="mt-1 text-xs text-grey-700">
              {d.paramState === 1 ? 'validated' : d.paramState === 2 ? 'mismatch' : 'unvalidated'}
            </div>
            <div className="text-[10px] font-mono text-grey-500">
              crc 0x{(d.paramCrc ?? 0).toString(16).toUpperCase().padStart(8, '0')}
            </div>
          </div>
        </div>

        {(d.faultSources ?? 0) !== 0 && (
          <div className="mt-3 flex flex-wrap items-center gap-1">
            <span className="text-[10px] uppercase tracking-wider text-grey-500 mr-1">runtime faults</span>
            {FAULT_SOURCE.map((name, bit) =>
              ((d.faultSources ?? 0) & (1 << bit)) !== 0 ? (
                <span key={bit} className="inline-flex items-center h-[16px] px-1 text-[9px] bg-status-bad text-white">{name}</span>
              ) : null,
            )}
            {(d.leaseError ?? 0) !== 0 && (
              <span className="inline-flex items-center h-[16px] px-1 text-[9px] border border-status-bad text-status-bad font-mono">
                lease 0x{(d.leaseError ?? 0).toString(16).toUpperCase()}
              </span>
            )}
          </div>
        )}
      </div>

      <p className="mt-2 text-[10px] text-grey-500 leading-relaxed">
        A tolerance change takes effect when the safety connection is next established, not while it
        is in Data: a write during safe operation must not move the reference a safety master is
        guarding against. The high-water marks and the counters reset with it, so each connection
        gets a clean diagnostic epoch.
      </p>
    </section>
  )
}
