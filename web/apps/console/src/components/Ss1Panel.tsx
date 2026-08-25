import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useConnection } from '../contexts/ConnectionContext'

/**
 * Safe Stop 1: its parameters, and enough diagnosis to tell a healthy stop from
 * a misconfigured one (ETG.6100.2 ch. 8.2).
 *
 * Reads over plain SDO like the safe-sensor panel, so it works with no safety
 * connection - which is when a parameter set is usually being chosen.
 *
 * The panel does NOT offer a button to command SS1. Starting a stop is a safety
 * master's job, and ch. 8.2.1.1 makes it irreversible once begun: releasing the
 * request does not abort the stop. A one-click "stop the axis" in a diagnostics
 * page would be a decision nobody could take back.
 */

/* AL states that have a working CoE mailbox: PRE-OP, SAFE-OP, OP. INIT has no mailbox at all and
   BOOT's speaks FoE, not CoE. Polling parameters across a firmware update - which is exactly when
   this panel is most likely to be left open - would otherwise keep issuing SDOs into the window
   where the drive's mailbox is being reprogrammed for BOOT. */
const MAILBOX_ACTIVE_STATES = new Set([2, 4, 8])

const OBJ = {
  tSS1: [0x6651, 1],       // t_SS1        [ms]  the unconditional deadline
  nZero: [0x6653, 1],      // n_Zero_SS1   [mRPM] standstill window, 0 = not monitored
  tL: [0x6654, 1],         // t_L_SS1      [ms]  dwell inside the window
  aSS1: [0x6656, 1],       // a_SS1        [milli-rev/s^2] 0 = no deceleration monitoring
  tD: [0x6657, 1],         // t_D_SS1      [ms]  delay before the limit is enforced
  command: [0x6650, 1],    // 1 while SS1 is active
  state: [0x2606, 1],      // 0 inactive, 1 stopping, 2 STO held
  paramsOk: [0x2606, 2],   // 0 = the written set was refused whole
  rampArmed: [0x2606, 3],
  rampLimit: [0x2606, 4],  // [mRPM] the ceiling the ramp imposes now
  stopDecel: [0x2606, 5],  // [mRPM/s] a copy of 0x6084 - the ramp the stop runs on
  velActual: [0x606c, 0],  // the drive's own velocity, for the estimates below
} as const

type ObjKey = keyof typeof OBJ
const SIGNED = new Set<ObjKey>(['rampLimit', 'velActual'])
const WRITABLE = new Set<ObjKey>(['tSS1', 'nZero', 'tL', 'aSS1', 'tD'])

const TONE_CLS = {
  ok: 'bg-status-good/10 text-status-good border border-status-good/30',
  info: 'bg-grey-100 text-grey-600 border border-grey-300',
  warn: 'bg-status-warn/10 text-status-warn border border-status-warn/40',
  bad: 'bg-status-bad/10 text-status-bad border border-status-bad/40',
} as const

const STATE: Record<number, { label: string; tone: keyof typeof TONE_CLS; hint: string }> = {
  0: { label: 'Inactive', tone: 'info', hint: 'No stop is running. Torque is permitted as far as SS1 is concerned.' },
  1: { label: 'Stopping', tone: 'warn', hint: 'A stop is in progress: the deceleration has been requested and the monitors are running. This state is LATCHED - releasing the request will not abort it.' },
  2: { label: 'STO held', tone: 'bad', hint: 'The stop finalized and torque is held off. It clears when the master releases the SS1 request.' },
}

function decodeLE(bytes: number[], signed: boolean): number {
  let v = 0
  for (let i = bytes.length - 1; i >= 0; i--) v = v * 256 + bytes[i]
  if (signed && bytes.length === 4 && v > 0x7fffffff) v -= 0x100000000
  if (signed && bytes.length === 2 && v > 0x7fff) v -= 0x10000
  return v
}

function Chip({ tone, children, hint }: { tone: keyof typeof TONE_CLS; children: React.ReactNode; hint?: string }) {
  return (
    <span title={hint} className={`inline-flex items-center h-[18px] px-1.5 text-[10px] tracking-wide ${hint ? 'cursor-help' : ''} ${TONE_CLS[tone]}`}>
      {children}
    </span>
  )
}

/**
 * The deceleration limit against the speed it is judging.
 *
 * The bar is the limit; the needle is the axis. While mode B is armed the limit
 * descends every cycle, so the needle drifting toward the right-hand edge is a
 * stop about to be declared a violation. Unarmed, the whole thing is greyed -
 * because a limit that is not being enforced tells you nothing, and showing it
 * as if it were would invite exactly the wrong conclusion.
 */
function RampBar({ limit, speed, armed }: { limit: number; speed: number; armed: boolean }) {
  if (!armed) {
    return (
      <div className="mt-2 text-[10px] text-grey-500">
        No deceleration limit is being enforced right now, so there is nothing to compare against.
      </div>
    )
  }
  const span = Math.max(limit, Math.abs(speed), 1)
  const pct = (v: number) => Math.max(0, Math.min(100, (v / span) * 100))
  const used = limit > 0 ? Math.abs(speed) / limit : 1
  const tone = used > 0.95 ? 'bg-status-bad' : used > 0.8 ? 'bg-status-warn' : 'bg-status-good'
  return (
    <div className="mt-3">
      <div className="relative h-[22px] border border-grey-300 bg-grey-50">
        <div className={`absolute top-0 bottom-0 ${tone} opacity-30`} style={{ left: 0, width: `${pct(limit)}%` }} />
        <div className="absolute top-0 bottom-0 w-px bg-status-bad" style={{ left: `${pct(limit)}%` }} title={`limit ${limit} mRPM`} />
        <div className="absolute top-0 bottom-0 w-[2px] bg-grey-900" style={{ left: `${pct(Math.abs(speed))}%` }} title={`speed ${Math.abs(speed)} mRPM`} />
      </div>
      <div className="mt-1 flex justify-between text-[9px] text-grey-500 font-mono">
        <span>0</span>
        <span className="text-grey-700">{Math.abs(speed)} mRPM against a {limit} limit</span>
      </div>
      <div className="text-[10px] text-grey-500">
        using <span className="font-mono text-grey-700">{(used * 100).toFixed(0)}%</span> of the limit
        {used > 1 ? ' - above it, which trips STO with an error' : ''}
      </div>
    </div>
  )
}

export default function Ss1Panel({ slavePosition }: { slavePosition: number }) {
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
    queryKey: ['ss1', slavePosition],
    queryFn: async () => {
      const out: Partial<Record<ObjKey, number>> = {}
      for (const [key, [index, sub]] of Object.entries(OBJ)) {
        const signed = SIGNED.has(key as ObjKey)
        let value: number | undefined
        try {
          const res = await api.readParameter(slavePosition, index, sub)
          value = Number(res.data.value)
        } catch {
          /* As in the safe-sensor panel: readParameter answers from the
             enumerated dictionary, which is empty right after a firmware update -
             exactly when somebody wants to look. An SDO upload asks the device. */
          try {
            const raw = await api.sdoUpload(slavePosition, index, sub)
            value = decodeLE(raw.data.data, signed)
          } catch { /* a device without SS1 simply does not answer; leave undefined */ }
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
    onSuccess: () => { setError(null); queryClient.invalidateQueries({ queryKey: ['ss1', slavePosition] }) },
    onError: (e: unknown) => setError(String(e)),
  })

  const d = q.data ?? {}
  const present = d.state !== undefined || d.tSS1 !== undefined

  if (q.isLoading) return <div className="border border-grey-200 p-4 text-sm text-grey-500">Reading the Safe Stop 1 objects…</div>
  if (!present) {
    return (
      <div className="border border-grey-200 p-4 text-sm text-grey-500">
        This device does not answer object 0x2606, so it does not implement Safe Stop 1.
      </div>
    )
  }

  const tSS1 = d.tSS1 ?? 0
  const nZero = d.nZero ?? 0
  const tL = d.tL ?? 0
  const aSS1 = d.aSS1 ?? 0
  const tD = d.tD ?? 0
  const decel = d.stopDecel ?? 0
  const speed = Math.abs(d.velActual ?? 0)

  const modeB = aSS1 > 0
  const paramsRefused = d.paramsOk === 0
  const inconsistent = modeB && tD >= tSS1

  /* How long the configured ramp needs to bring the axis to the standstill
     window from where it is now. This is the number that decides whether a stop
     completes or times out, and it is not otherwise visible anywhere. */
  const rampMs = decel > 0 ? ((speed - nZero) / decel) * 1000 : Infinity
  const deadlineTooShort = Number.isFinite(rampMs) && rampMs > 0 && tSS1 > 0 && rampMs + tL > tSS1
  const noDecel = decel === 0

  const numEdit = (key: ObjKey) => edits[key] ?? String(d[key] ?? '')
  const commit = (key: ObjKey) => {
    const v = Number(edits[key])
    if (!Number.isFinite(v)) return
    write.mutate({ key, value: Math.round(v) })
    setEdits(e => ({ ...e, [key]: undefined }))
  }

  const Field = ({ k, label, unit, hint }: { k: ObjKey; label: string; unit: string; hint: string }) => (
    <div className="flex items-end gap-2">
      <label className="flex-1">
        <span className="block text-[10px] uppercase tracking-wider text-grey-500" title={hint}>
          {label} <span className="text-grey-400">({unit})</span>
        </span>
        <input
          className="border border-grey-300 px-2 h-[30px] text-xs w-full bg-white font-mono"
          value={numEdit(k)}
          disabled={!WRITABLE.has(k)}
          onChange={e => setEdits(x => ({ ...x, [k]: e.target.value }))}
          onKeyDown={e => { if (e.key === 'Enter') commit(k) }}
        />
      </label>
      <button className="border border-grey-300 text-grey-700 px-2 h-[30px] text-[11px] hover:bg-grey-50 cursor-pointer"
              disabled={write.isPending} onClick={() => commit(k)}>Set</button>
    </div>
  )

  return (
    <div className="border border-grey-200">
      <div className="px-4 py-2 border-b border-grey-200 flex items-center gap-2 flex-wrap">
        <span className="text-xs uppercase tracking-wider text-grey-700">Safe Stop 1</span>
        <Chip tone={STATE[d.state ?? 0]?.tone ?? 'info'} hint={STATE[d.state ?? 0]?.hint}>
          {STATE[d.state ?? 0]?.label ?? `State ${d.state}`}
        </Chip>
        <Chip tone={modeB ? 'ok' : 'info'}
              hint={modeB
                ? 'a_SS1 is set, so the deceleration is monitored against a descending limit as well as against the deadline (ch. 8.2.1.2).'
                : 'a_SS1 is 0, so only the deadline and the optional standstill window apply (ch. 8.2.1.1). This is the standard default.'}>
          {modeB ? 'Time + deceleration' : 'Time monitoring'}
        </Chip>
        {d.command === 1 && (
          <Chip tone="warn" hint="Object 0x6650:01 - the master is requesting SS1, or a stop is still holding STO.">
            requested
          </Chip>
        )}
        {paramsRefused && (
          <Chip tone="bad" hint="0x2606:02 - the parameter set was refused as a whole because its members disagree. The previous set is still in force.">
            set refused
          </Chip>
        )}
      </div>

      <div className="p-4 grid grid-cols-1 md:grid-cols-2 gap-6">
        {/* ---------------- configuration ---------------- */}
        <div>
          <div className="text-[10px] uppercase tracking-wider text-grey-500 mb-2">Parameters</div>
          <div className="space-y-3">
            <Field k="tSS1" label="Time to STO — t_SS1" unit="ms"
                   hint="0x6651:01. The unconditional deadline: torque is removed at the latest after this time, whatever the axis did. 0 makes SS1 behave exactly as STO." />
            <Field k="nZero" label="Standstill window — n_Zero_SS1" unit="mRPM"
                   hint="0x6653:01. Applied as +/- this value. Reaching it for t_L_SS1 removes torque EARLY; it can never delay the deadline. 0 disables the window, and with no window a deadline expiry is not an error." />
            <Field k="tL" label="Dwell in window — t_L_SS1" unit="ms"
                   hint="0x6654:01. The dwell must be contiguous: leaving the window restarts it, so an oscillating axis cannot accumulate credit it never held." />
            <Field k="aSS1" label="Deceleration limit — a_SS1" unit="milli-rev/s²"
                   hint="0x6656:01. The MINIMUM deceleration the axis must sustain. Compared strictly, with no tolerance band, so size it with margin. 0 disables deceleration monitoring." />
            <Field k="tD" label="Monitoring delay — t_D_SS1" unit="ms"
                   hint="0x6657:01. How long after activation the limit starts being enforced. Must be strictly lower than t_SS1." />
          </div>

          {inconsistent && (
            <div className="mt-3 border border-status-bad/40 bg-status-bad/5 px-3 py-2 text-[11px] text-status-bad">
              t_D_SS1 ({tD} ms) is not lower than t_SS1 ({tSS1} ms). The specification requires it to be
              lower, so this set is refused whole and deceleration monitoring stays off — time
              monitoring remains in force rather than the drive guessing.
            </div>
          )}
        </div>

        {/* ---------------- what a stop will actually do ---------------- */}
        <div>
          <div className="text-[10px] uppercase tracking-wider text-grey-500 mb-2">What a stop will do</div>

          <div className="space-y-1 text-[11px]">
            <div className="flex justify-between">
              <span className="text-grey-600" title="0x2606:05 - a copy of 0x6084. SS1 commands zero velocity and the profiler brings the demand down at this rate. It is NOT the quick-stop ramp.">
                Deceleration in force
              </span>
              <span className="font-mono text-grey-800">{decel} mRPM/s</span>
            </div>
            <div className="flex justify-between">
              <span className="text-grey-600">Axis velocity now</span>
              <span className="font-mono text-grey-800">{speed} mRPM</span>
            </div>
            <div className="flex justify-between">
              <span className="text-grey-600" title="How long that ramp needs to bring the axis from its current speed into the standstill window.">
                Ramp needs
              </span>
              <span className="font-mono text-grey-800">
                {Number.isFinite(rampMs) ? `${Math.max(0, rampMs).toFixed(0)} ms` : '—'}
              </span>
            </div>
            <div className="flex justify-between">
              <span className="text-grey-600">Deadline allows</span>
              <span className="font-mono text-grey-800">{tSS1} ms</span>
            </div>
          </div>

          {noDecel && (
            <div className="mt-3 border border-status-bad/40 bg-status-bad/5 px-3 py-2 text-[11px] text-status-bad">
              The deceleration in force is <span className="font-mono">0</span>, so an SS1 stop will not
              slow the axis at all — it will coast until the deadline and then lose torque. Set
              0x6084 (profile deceleration) to give the stop a ramp.
            </div>
          )}

          {!noDecel && deadlineTooShort && (
            <div className="mt-3 border border-status-warn/40 bg-status-warn/5 px-3 py-2 text-[11px] text-status-warn">
              The ramp needs about <span className="font-mono">{rampMs.toFixed(0)} ms</span> plus a{' '}
              <span className="font-mono">{tL} ms</span> dwell, but t_SS1 allows{' '}
              <span className="font-mono">{tSS1} ms</span>. The standstill window cannot be reached in
              time, so every stop from this speed will hit the deadline and report the
              ETG.6100.2 Table 13 error even though nothing is broken. Either lengthen t_SS1 or
              steepen 0x6084.
            </div>
          )}

          <div className="mt-4 text-[10px] uppercase tracking-wider text-grey-500 mb-1">Deceleration limit</div>
          <RampBar limit={d.rampLimit ?? 0} speed={speed} armed={d.rampArmed === 1} />

          {modeB && !inconsistent && (
            <div className="mt-3 text-[10px] text-grey-500">
              The specification sizes this limit as{' '}
              <span className="font-mono text-grey-700">|a_SS1| ≥ |n_max| / (t_SS1 − t_D_SS1)</span>.
              {tSS1 > tD && speed > 0 && (
                <>
                  {' '}At the present speed that is at least{' '}
                  <button
                    className="font-mono underline decoration-dotted cursor-pointer hover:text-grey-800"
                    title="Put this minimum into the a_SS1 field. It is a floor, not a recommendation - add margin, because the comparison carries no tolerance band."
                    onClick={() => setEdits(x => ({
                      ...x,
                      aSS1: String(Math.ceil((speed / 60) / ((tSS1 - tD) / 1000))),
                    }))}
                  >
                    {Math.ceil((speed / 60) / ((tSS1 - tD) / 1000))}
                  </button>
                  {' '}milli-rev/s².
                </>
              )}
            </div>
          )}
        </div>
      </div>

      {error && <div className="px-4 pb-3 text-[11px] text-status-bad break-all">{error}</div>}

      <div className="px-4 py-2 border-t border-grey-200 text-[10px] text-grey-500">
        SS1 is commanded on safety controlword bit 1, and there is no SS1 bit in the safety
        statusword — bit 1 there is Safe Speed Monitor. A stop therefore shows up as STO appearing
        later than it was asked for, which is why these objects exist. Once activated a stop must
        finalize: releasing the request does not abort it.
      </div>
    </div>
  )
}
