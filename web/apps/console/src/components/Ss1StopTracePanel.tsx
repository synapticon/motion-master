import { useMemo } from 'react'
import { useQuery } from '@tanstack/react-query'
import type uPlot from 'uplot'
import { useConnection } from '../contexts/ConnectionContext'
import MonitoringChart, { SINCE_TRIGGER, type ChartAnnotations } from './MonitoringChart'
import Section from './Section'

/**
 * The last Safe Stop 1 stop, plotted against the limits it was judged by.
 *
 * The samples come from the FSoE cycle thread, not from polling: a stop completes in a couple of
 * hundred milliseconds and the finalizing STO is visible for a single cycle, so anything sampled
 * from here would miss it. The parameters that make up the overlays are read over SDO, because they
 * are commissioning values that do not change during a stop and reading them server-side would put
 * bus traffic behind a plot refresh.
 *
 * The point of the picture is margin. The objects can tell you a stop passed or tripped; they cannot
 * tell you by how much, because the deceleration limit at 0x2606:04 is a per-cycle value that reads
 * 0 on every cycle that is not an armed mode-B cycle - and the armed window is often under 100 ms,
 * so an SDO poll essentially never lands inside it.
 */

const PARAM = {
  tSS1: [0x6651, 1],
  nZero: [0x6653, 1],
  tL: [0x6654, 1],
  aSS1: [0x6656, 1],
  tD: [0x6657, 1],
  paramsOk: [0x2606, 2],
  stopDecel: [0x2606, 5],
  fwAnchor: [0x2606, 6],
  fwAnchorValid: [0x2606, 7],
  safetyCycleUs: [0x2605, 4],
} as const

type ParamKey = keyof typeof PARAM

function decodeLE(bytes: number[], signed = false): number {
  let v = 0
  for (let i = bytes.length - 1; i >= 0; i--) v = v * 256 + bytes[i]
  if (signed && bytes.length === 4 && v > 0x7fffffff) v -= 0x100000000
  return v
}

const fmtMs = (us: number) => `${(us / 1000).toFixed(us < 10000 ? 1 : 0)} ms`
const fmtRpm = (mrpm: number) => `${(mrpm / 1000).toFixed(mrpm < 10000 ? 2 : 0)} rpm`

export default function Ss1StopTracePanel({ slavePosition }: { slavePosition: number }) {
  const { api } = useConnection()

  const trace = useQuery({
    queryKey: ['ss1-trace', slavePosition],
    queryFn: async () => (await api.readSs1Trace(slavePosition)).data,
    // Faster while a stop is being recorded, so the plot appears promptly after it finishes; slow
    // otherwise, because a trace that is not changing does not need to be re-fetched.
    refetchInterval: (q) => (q.state.data?.capturing ? 400 : 2000),
    retry: false,
  })

  const params = useQuery({
    queryKey: ['ss1-trace-params', slavePosition],
    queryFn: async () => {
      const out: Partial<Record<ParamKey, number>> = {}
      for (const [key, [index, sub]] of Object.entries(PARAM)) {
        try {
          const res = await api.readParameter(slavePosition, index, sub)
          out[key as ParamKey] = Number(res.data.value)
        } catch {
          try {
            const raw = await api.sdoUpload(slavePosition, index, sub)
            out[key as ParamKey] = decodeLE(raw.data.data)
          } catch { /* absent object: leave undefined so the panel can say so */ }
        }
      }
      return out
    },
    refetchInterval: 5000,
  })

  const t = trace.data
  const p = params.data ?? {}
  const standstillStops = (t as { standstillStops?: number } | undefined)?.standstillStops ?? 0

  const model = useMemo(() => {
    if (!t?.haveTrace || !t.samples?.length || !t.columns?.length) return null
    const col = (name: string) => t.columns!.indexOf(name)
    const iT = col('tUs')
    const iSpeed = col('safeSpeedMilliRpm')
    const iPos = col('safePositionRevolutions')
    const iVelValid = col('velocityValid')
    const iSto = col('stoActive')
    if (iT < 0 || iSpeed < 0) return null

    const rows = t.samples as number[][]
    const xs = rows.map((r) => r[iT])
    const speed = rows.map((r) => r[iSpeed])

    const T = p.safetyCycleUs && p.safetyCycleUs > 0 ? p.safetyCycleUs : 1000
    const A = p.aSS1 ?? 0
    const tSS1Us = (p.tSS1 ?? 0) * 1000
    const tDUs = (p.tD ?? 0) * 1000
    const nZero = p.nZero ?? 0
    /* Prefer the anchor the FIRMWARE latched (0x2606:06) over the one the recorder inferred. They
       normally agree, and when they do not the firmware's is the one the limit was actually enforced
       against - drawing a line from a different anchor is indistinguishable from the monitor being
       broken, which is a mistake worth not making twice. */
    const anchor = p.fwAnchor && p.fwAnchor > 0 ? p.fwAnchor : (t.anchorMilliRpm ?? 0)
    const anchorValid = p.fwAnchorValid !== undefined ? p.fwAnchorValid === 1 : !!t.anchorValid

    /* The limit line, ported from sdp_ss1_update rather than approximated. It is a STAIRCASE on the
       safety cycle, not a straight line, and that is what decides a trip: at a_SS1 = 6e6 the step is
       360000 mRPM per cycle. The floor is applied to the CUMULATIVE product, which is what
       reproduces the firmware's carried remainder - truncating per step instead loses the whole
       descent at small a_SS1, which is the drift bug its own unit test pins.

       Drawn from the SAFETY cycle period (0x2605:04), not the bus period: SIM runs 1 ms and AM2612
       pumps the safety task at 250 us, so assuming they are equal would draw the wrong slope. */
    const modeB = A > 0 && (p.tD ?? 0) < (p.tSS1 ?? 0) && anchorValid
    const k0 = Math.max(1, Math.ceil(tDUs / T))
    const origin = (k0 - 1) * T
    const limitAt = (tUs: number): number | null => {
      if (!modeB || tUs < origin) return null
      const n = Math.floor((tUs - origin) / T) + 1
      const drop = Math.floor((A * 60 * T * n) / 1_000_000)
      const lim = Math.max(0, anchor - drop)
      return lim > nZero ? lim : null
    }
    const limit = xs.map(limitAt)

    /* What the profiler was asked to do, for comparison: SS1 commands zero and the demand comes down
       at 0x6084. Where the measured speed lags this, the lag is the plant plus the safe-velocity
       filter - and seeing that is what stops someone sizing a_SS1 against the plant instead of
       against what the monitor can see. */
    const decel = p.stopDecel ?? 0
    const expected = xs.map((x) =>
      x < 0 || decel <= 0 ? null : Math.max(0, anchor - (decel * x) / 1_000_000))

    // --- the numbers that are the actual point of the plot ---
    let headroom: number | null = null
    for (let i = 0; i < xs.length; i++) {
      const l = limit[i]
      if (l == null) continue
      const margin = l - speed[i]
      if (headroom == null || margin < headroom) headroom = margin
    }

    const stoUs = t.markers?.stoActiveTUs ?? null
    let distanceRev: number | null = null
    if (iPos >= 0) {
      const at0 = rows.findIndex((r) => r[iT] >= 0)
      const end = stoUs == null ? rows.length - 1 : rows.findIndex((r) => r[iT] >= stoUs)
      if (at0 >= 0 && end > at0) distanceRev = Math.abs(rows[end][iPos] - rows[at0][iPos])
    }

    /* Dead time: how long after the request the speed first fell by more than the noise the trace
       already shows. It contains the FSoE round trip, the safe-velocity filter window and the
       profiler's reaction, and no object exposes any of it. */
    let deadTimeUs: number | null = null
    const pre = rows.filter((r) => r[iT] < 0).map((r) => r[iSpeed])
    if (pre.length > 4) {
      const mean = pre.reduce((a, b) => a + b, 0) / pre.length
      const noise = Math.max(...pre.map((v) => Math.abs(v - mean))) || 1
      for (const r of rows) {
        if (r[iT] < 0) continue
        if (mean - r[iSpeed] > noise * 3) { deadTimeUs = r[iT]; break }
      }
    }

    // --- overlays ---
    const vLines: NonNullable<ChartAnnotations['vLines']> = [
      { x: 0, label: 'SS1', color: '#2b6cb0' },
    ]
    if (tSS1Us > 0) vLines.push({ x: tSS1Us, label: 't_SS1', color: '#e0004d', dash: [4, 3] })
    if (modeB && tDUs > 0) vLines.push({ x: tDUs, label: 't_D', color: '#805ad5', dash: [2, 3] })
    if (stoUs != null) vLines.push({ x: stoUs, label: `STO ${fmtMs(stoUs)}`, color: '#c05621' })

    const vBands: NonNullable<ChartAnnotations['vBands']> = []
    /* One cycle of the bus plus one of the safety task: the request is not on the wire until the
       next exchange and the drive picks it up within its own period. A hairline at zero would claim
       precision the data does not have. */
    vBands.push({ from: 0, to: (t.measuredCyclePeriodUs ?? 1000) + T, color: 'rgba(43,108,176,0.10)' })
    // Where the safe velocity was not believable, no violation could be declared even though the
    // limit kept descending. Unshaded, the plot would imply enforcement that was suspended.
    if (iVelValid >= 0) {
      let start: number | null = null
      rows.forEach((r, i) => {
        const bad = r[iVelValid] === 0
        if (bad && start == null) start = r[iT]
        if (!bad && start != null) { vBands.push({ from: start, to: r[iT], color: 'rgba(245,158,11,0.14)' }); start = null }
        if (bad && i === rows.length - 1 && start != null) vBands.push({ from: start, to: r[iT], color: 'rgba(245,158,11,0.14)' })
      })
    }

    const hBands = nZero > 0 ? [{ from: 0, to: nZero, color: 'rgba(16,185,129,0.14)' }] : []

    /* uPlot legends only list series, and every shaded area here is an annotation - so without this
       the reader is left to match three washes of colour against a paragraph of prose. Only the
       regions actually drawn are listed.

       `swatch` is the band colour at a higher alpha, with a solid border of the same hue: at the
       0.10-0.14 alpha that works over a plot, a 12px square is nearly invisible against white. The
       shape carries the meaning - filled square for an area, dash for a line - and the hue carries
       the identity. */
    const regions: { swatch: string; border: string; label: string; hint: string }[] = [
      {
        swatch: 'rgba(43,108,176,0.28)',
        border: 'rgba(43,108,176,0.65)',
        label: 'activation uncertainty',
        hint: 'One bus cycle plus one safety cycle after the request. The controlword is not on the wire until the next exchange and the drive picks it up within its own period, so the exact moment the stop began is not knowable to finer than this.',
      },
    ]
    if (vBands.length > 1) {
      regions.push({
        swatch: 'rgba(245,158,11,0.30)',
        border: 'rgba(245,158,11,0.70)',
        label: 'safe velocity not believable',
        hint: 'Cycles where the velocity validity bit was clear. No violation could be declared across these even though the limit kept descending, so enforcement was suspended - unshaded, the plot would imply it was not.',
      })
    }
    if (nZero > 0) {
      regions.push({
        swatch: 'rgba(16,185,129,0.30)',
        border: 'rgba(16,185,129,0.70)',
        label: 'standstill window (n_Zero_SS1)',
        hint: 'Plus and minus n_Zero_SS1. Holding the speed inside this band for t_L_SS1 removes torque early; it can never delay the deadline.',
      })
    }

    const points: NonNullable<ChartAnnotations['points']> = []
    if (stoUs != null && iSto >= 0) {
      const at = rows.find((r) => r[iT] >= stoUs)
      if (at) points.push({ x: stoUs, y: at[iSpeed], color: '#c05621' })
    }

    /* The outcome, in the firmware's own evaluation order - the violation is tested before the dwell
       and before the deadline, so it wins a tie. */
    const errUs = t.markers?.errorTUs ?? null
    let outcome: { label: string; tone: 'ok' | 'warn' | 'bad'; hint: string }
    if (stoUs == null) {
      outcome = { label: 'no torque removal recorded', tone: 'warn', hint: 'The capture ended before STO was observed - see the end reason.' }
    } else if (errUs != null && modeB && stoUs >= origin && stoUs < tSS1Us - T) {
      outcome = { label: 'deceleration limit violated', tone: 'bad', hint: 'The measured speed rose above the a_SS1 ramp, so STO was initiated early with the Error bit. ETG.6100.2 Table 13.' }
    } else if (tSS1Us > 0 && Math.abs(stoUs - tSS1Us) <= 2 * T) {
      outcome = errUs != null
        ? { label: 'deadline expired without standstill', tone: 'bad', hint: 'The standstill window was configured and was not satisfied by t_SS1, so the stop reports an error even though torque was removed. Either t_SS1 is too short or the ramp is too shallow.' }
        : { label: 'deadline expired', tone: 'warn', hint: 'Torque came off on the deadline. With no standstill window configured that is not an error, but nothing confirmed the axis had stopped.' }
    } else {
      outcome = { label: 'standstill reached', tone: 'ok', hint: 'The axis entered the standstill window and held it for the dwell, so torque was removed early and cleanly.' }
    }

    const chart: uPlot.AlignedData = [xs, speed, limit, expected] as unknown as uPlot.AlignedData
    return { chart, annotations: { vLines, vBands, hBands, points }, regions, headroom, distanceRev, deadTimeUs, outcome, modeB, stoUs, T }
  }, [t, p])

  if (trace.isError) {
    return (
      <div className="border border-grey-200 p-4 text-sm text-grey-500">
        No safety connection is open to this device, so there is no stop recorder running.
      </div>
    )
  }
  if (trace.isLoading) {
    return <div className="border border-grey-200 p-4 text-sm text-grey-500">Reading the stop recorder…</div>
  }

  const shortInputs = (t?.safeInputsLength ?? 12) < 12

  return (
    <Section
      title="Last SS1 stop"
      chips={
        <>
        {t?.capturing && (
          <span className="inline-flex items-center h-[18px] px-1.5 text-[10px] bg-status-warn/10 text-status-warn border border-status-warn/40">
            recording
          </span>
        )}
        {t?.haveTrace && (
          <>
            <span className="text-[10px] text-grey-500 font-mono">#{t.traceId}</span>
            {model && (
              <span title={model.outcome.hint}
                    className={`inline-flex items-center h-[18px] px-1.5 text-[10px] cursor-help ${
                      model.outcome.tone === 'ok' ? 'bg-status-good/10 text-status-good border border-status-good/30'
                      : model.outcome.tone === 'warn' ? 'bg-status-warn/10 text-status-warn border border-status-warn/40'
                      : 'bg-status-bad/10 text-status-bad border border-status-bad/40'}`}>
                {model.outcome.label}
              </span>
            )}
            {t.endReason && t.endReason !== 'StoObserved' && (
              <span title="The capture was cut short, so the trace is not a whole stop."
                    className="inline-flex items-center h-[18px] px-1.5 text-[10px] border border-grey-300 text-grey-600 cursor-help">
                {t.endReason}
              </span>
            )}
            {standstillStops > 0 && (
              <span title="SS1 was requested this many times with the axis already still. Those stops have no deceleration in them - every sample would sit inside the standstill window - so they are counted rather than allowed to replace the trace below."
                    className="inline-flex items-center h-[18px] px-1.5 text-[10px] border border-grey-300 text-grey-600 cursor-help">
                +{standstillStops} from standstill
              </span>
            )}
            {p.paramsOk === 0 && (
              <span title="0x2606:02 reports the written parameter set was refused, so the overlays may not be the set that was actually enforced during this stop."
                    className="inline-flex items-center h-[18px] px-1.5 text-[10px] bg-status-bad/10 text-status-bad border border-status-bad/40 cursor-help">
                parameters refused
              </span>
            )}
          </>
        )}
        </>
      }
    >
      {shortInputs ? (
        <div className="p-4 text-sm text-grey-500">
          This connection carries {t?.safeInputsLength ?? 0} SafeInputs octets, so it does not publish
          a safe velocity. There is nothing to plot — and drawing it anyway would show a stop from
          zero, which is the worst available lie about a safety function.
        </div>
      ) : !t?.haveTrace ? (
        <div className="p-4 text-sm text-grey-500">
          No stop has been recorded since this connection opened. Request SS1 and the trace appears
          here — it is kept server-side, so you can navigate away and come back to it.
        </div>
      ) : !model ? (
        <div className="p-4 text-sm text-grey-500">The trace carries no usable samples.</div>
      ) : (
        <>
          <div className="px-4 pt-3 grid grid-cols-2 md:grid-cols-4 gap-3 text-[11px]">
            <div>
              <div className="text-[10px] uppercase tracking-wider text-grey-500"
                   title="The smallest gap between the measured speed and the a_SS1 limit across the whole armed span. This is the number the objects cannot give you: 0x2606:04 is a per-cycle value that reads 0 outside an armed mode-B cycle, and the armed window is often under 100 ms.">
                Limit headroom
              </div>
              <div className="font-mono text-grey-800">
                {model.headroom == null ? '—' : fmtRpm(model.headroom)}
              </div>
            </div>
            <div>
              <div className="text-[10px] uppercase tracking-wider text-grey-500"
                   title="Output-shaft revolutions travelled between the request and torque removal, from the safe position channel. A guard-distance calculation needs the measured value, not the configured one.">
                Stopping distance
              </div>
              <div className="font-mono text-grey-800">
                {model.distanceRev == null ? '—' : `${model.distanceRev.toFixed(4)} rev`}
              </div>
            </div>
            <div>
              <div className="text-[10px] uppercase tracking-wider text-grey-500"
                   title="From the request to the first fall in speed beyond the trace's own noise. Contains the FSoE round trip, the safe-velocity filter window and the profiler's reaction - none of which any object exposes.">
                Reaction dead time
              </div>
              <div className="font-mono text-grey-800">
                {model.deadTimeUs == null ? '—' : fmtMs(model.deadTimeUs)}
              </div>
            </div>
            <div>
              <div className="text-[10px] uppercase tracking-wider text-grey-500"
                   title="From the SS1 request to the cycle where the statusword reported STO active.">
                Time to torque off
              </div>
              <div className="font-mono text-grey-800">
                {model.stoUs == null ? '—' : fmtMs(model.stoUs)}
              </div>
            </div>
          </div>

          <div className="px-2 pt-2">
            <MonitoringChart
              data={model.chart}
              xAxis={SINCE_TRIGGER}
              labels={['Safe speed', 'Deceleration limit (a_SS1)', 'Expected demand (0x6084)']}
              colors={['#00849b', '#e0004d', '#9ca3af']}
              dashes={[undefined, [4, 4], [2, 4]]}
              annotations={model.annotations}
              titles={[
                'The magnitude of the safe velocity - what the firmware actually compares.',
                'The descending limit, computed on the safety cycle exactly as the firmware does. Absent when a_SS1 is 0, the parameter set is inconsistent, or no speed could be latched at activation.',
                'What the Profile Velocity profiler was asked to do at 0x6084. The gap to the measured speed is the plant plus the safe-velocity filter lag.',
              ]}
            />
          </div>

          {(t.anchorMilliRpm ?? 0) < 1000 && (
            <div className="mx-4 mt-3 border border-grey-300 bg-grey-50 px-3 py-2 text-[11px] text-grey-600">
              This stop began with the axis already still, so there is no deceleration in it - every
              sample sits inside the standstill window and the trace is measurement noise around
              zero. It is shown because it is the only stop recorded so far. Request SS1 while the
              axis is moving and that one will be kept instead; a later stop from standstill will not
              replace it.
            </div>
          )}

          {/* The shaded areas, which uPlot's own legend cannot list: it knows about series, and
              these are annotations. Filled square for a region, against the dashes uPlot draws for
              the lines - the shape says which kind of thing it is before the colour says which. */}
          <div className="px-4 pt-1 pb-2 flex flex-wrap items-center gap-x-5 gap-y-1 text-[10px] text-grey-600">
            <span className="uppercase tracking-wider text-grey-400">Regions</span>
            {model.regions.map(r => (
              <span key={r.label} title={r.hint} className="inline-flex items-center gap-1.5 cursor-help">
                <span
                  className="inline-block w-3 h-3 border"
                  style={{ backgroundColor: r.swatch, borderColor: r.border }}
                  aria-hidden
                />
                {r.label}
              </span>
            ))}
          </div>

          {!model.modeB && (
            <div className="px-4 pb-2 text-[10px] text-grey-500">
              No deceleration limit is drawn: a_SS1 is 0, so only the deadline and the standstill
              window applied.
            </div>
          )}
        </>
      )}
    </Section>
  )
}
