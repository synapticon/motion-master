import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import {
  apiErrorMessage,
  type EncoderConfiguration,
  type IcMuCalibrationCurve,
  type IcMuCalibrationIteration,
  type IcMuCalibrationPlan,
  type IcMuCalibrationValidation,
  type ProcedureSnapshot,
  type ProgressStep,
} from '@synapticon/motion-master-client'
import Callout from './Callout'
import Checkbox from './Checkbox'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline, btnPrimary } from '../utils/styles'

const PROCEDURE = 'ic-mu-calibration'

const inputCls = 'border border-grey-300 px-3 h-[38px] text-sm bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'

const BAND_CLASS: Record<IcMuCalibrationValidation['marginBand'], string> = {
  robust: 'text-status-good',
  'small-displacements': 'text-status-info',
  'avoid-displacements': 'text-status-warn',
  'too-high': 'text-status-bad',
}

const STEP_DOT: Record<ProgressStep['status'], string> = {
  idle: 'bg-grey-300',
  running: 'bg-ocean',
  succeeded: 'bg-green-600',
  failed: 'bg-syn-red',
}

function stepValue<T>(snapshot: ProcedureSnapshot | undefined, id: string): T | undefined {
  return snapshot?.steps.find(step => step.id === id)?.value as T | undefined
}

// The iterations a run has finished, in order.
function iterations(snapshot: ProcedureSnapshot): IcMuCalibrationIteration[] {
  return snapshot.steps
    .filter(step => step.id.startsWith('iteration-') && step.status === 'succeeded')
    .map(step => step.value as IcMuCalibrationIteration)
}

/**
 * The calibration of one iC-MU encoder: the two ways to run it, and the last run of this encoder
 * with its phase error and, after the validation, its margin.
 */
export default function IcMuCalibrationSection({
  slavePosition,
  encoder,
  busy,
}: {
  slavePosition: number
  encoder: EncoderConfiguration
  busy: boolean
}) {
  const { api } = useConnection()
  const queryClient = useQueryClient()
  const [health, setHealth] = useState(false)
  const [ringPosition, setRingPosition] = useState<'inner' | 'outer'>('inner')
  const external = encoder.sensorPort >= 3
  const key = ['procedure', slavePosition, PROCEDURE]

  const snapshotQuery = useQuery({
    queryKey: key,
    queryFn: () => api.getProcedure(slavePosition, PROCEDURE).then(r => r.data),
    refetchInterval: query => (query.state.data?.status === 'running' ? 500 : false),
    refetchIntervalInBackground: true,
    retry: false,
  })
  const start = useMutation({
    mutationFn: (measurementOnly: boolean) =>
      api.startProcedure(slavePosition, PROCEDURE, {
        encoderOrdinal: encoder.encoderOrdinal,
        measurementOnly,
        ...(measurementOnly ? {} : { activateHealthMonitoring: health }),
        ...(external ? { ringPosition } : {}),
      }),
    onSettled: () => queryClient.invalidateQueries({ queryKey: key }),
  })
  const cancel = useMutation({
    mutationFn: () => api.cancelProcedure(slavePosition, PROCEDURE),
    onSettled: () => queryClient.invalidateQueries({ queryKey: key }),
  })

  const snapshot = snapshotQuery.data
  const plan = stepValue<IcMuCalibrationPlan>(snapshot, 'validate')
  // A run of another encoder is shown on that encoder's card, not here.
  const mine = snapshot && snapshot.status !== 'idle' && plan?.encoderOrdinal === encoder.encoderOrdinal
  const running = snapshot?.status === 'running'

  return (
    <div className="px-5 py-4 border-t border-grey-100 space-y-4">
      <h4 className="font-display uppercase tracking-wide text-xs text-grey-700">Calibration</h4>
      <p className="text-xs text-grey-600 max-w-3xl">
        Calibrates the master and the nonius track and the nonius table. The motor turns several
        revolutions in both directions at constant velocity. Tune the velocity controller first,
        with a bandwidth below 30 Hz, and keep the axis free to move within its software position
        limits. Start validation reports the margin of the calibration the encoder holds and writes
        nothing. Start calibration writes the EEPROM of the encoder; when the encoder is the
        commutation source, measure the commutation offset again afterwards.
      </p>
      <div className="flex flex-wrap items-end gap-3">
        {external && (
          <div>
            <label className={labelCls} htmlFor={`ring-${encoder.encoderOrdinal}`}>
              Ring position
            </label>
            <select
              id={`ring-${encoder.encoderOrdinal}`}
              className={inputCls}
              value={ringPosition}
              onChange={event => setRingPosition(event.target.value as 'inner' | 'outer')}
            >
              <option value="inner">Inner</option>
              <option value="outer">Outer</option>
            </select>
          </div>
        )}
        <label className="flex items-center gap-2 h-[38px] text-sm">
          <Checkbox checked={health} onChange={setHealth} disabled={busy || running} />
          Health monitoring
        </label>
        <button
          type="button"
          className={`${btnOutline} h-[38px]`}
          disabled={busy || running || start.isPending}
          onClick={() => start.mutate(true)}
        >
          Start validation
        </button>
        <button
          type="button"
          className={`${btnPrimary} h-[38px]`}
          disabled={busy || running || start.isPending}
          onClick={() => start.mutate(false)}
        >
          Start calibration
        </button>
        {mine && running && (
          <button
            type="button"
            className={`${btnOutline} h-[38px]`}
            disabled={cancel.isPending}
            onClick={() => cancel.mutate()}
          >
            Stop
          </button>
        )}
      </div>
      {start.isError && <Callout variant="error">{apiErrorMessage(start.error)}</Callout>}

      {mine && snapshot && <CalibrationRun snapshot={snapshot} plan={plan} />}
    </div>
  )
}

function CalibrationRun({
  snapshot,
  plan,
}: {
  snapshot: ProcedureSnapshot
  plan: IcMuCalibrationPlan | undefined
}) {
  const done = iterations(snapshot)
  const validation = stepValue<IcMuCalibrationValidation>(snapshot, 'validation')
  const failed = snapshot.steps.find(step => step.status === 'failed')
  const warnings = [
    ...(plan?.warnings ?? []),
    ...done.flatMap(i => (i.warning ? [i.warning] : [])),
    ...(validation?.commutationWarning ? [validation.commutationWarning] : []),
  ]
  const curves: Curve[] = []
  if (done.length > 0) {
    curves.push({ label: 'Iteration 1', color: CURVE_COLOR.first, points: done[0].phaseError })
  }
  if (done.length > 1) {
    const last = done[done.length - 1]
    curves.push({ label: `Iteration ${last.iteration}`, color: CURVE_COLOR.latest, points: last.phaseError })
  }
  if (validation) {
    curves.push({
      label: 'Validation',
      color: CURVE_COLOR.validation,
      points: validation.phaseMargin,
      worst: validation.marginPercent,
    })
  }
  const limit = validation?.phaseRangeLimit ?? done[0]?.phaseRangeLimit

  return (
    <div className="space-y-3">
      <p className="text-xs text-grey-600">
        Last run: {snapshot.status}
        {plan &&
          ` · ${plan.part}, ${plan.masterPolePairs} pole pairs, ${plan.ringPosition} ring, ` +
            `${plan.velocityRpm.toFixed(2)} rpm on the driving shaft for ${plan.recordingMs / 1000} s`}
      </p>
      {failed?.error && <Callout variant="error">{failed.error}</Callout>}
      {warnings.map(w => (
        <Callout key={w} variant="warning">
          {w}
        </Callout>
      ))}
      {validation && (
        <p className={`text-sm ${BAND_CLASS[validation.marginBand]}`}>
          Margin {validation.marginPercent.toFixed(1)} % (lower {validation.lowerMarginPercent.toFixed(1)} %,
          upper {validation.upperMarginPercent.toFixed(1)} %). {validation.marginMessage}
        </p>
      )}
      {curves.length > 0 && limit !== undefined && <PhasePlot curves={curves} limit={limit} />}
      <table className="w-full text-xs">
        <tbody>
          {snapshot.steps
            .filter(step => step.status !== 'idle')
            .map(step => (
              <tr key={step.id} className="border-b border-grey-100 last:border-0 align-top">
                <td className="py-1.5 w-5">
                  <span
                    className={`inline-block h-2 w-2 rounded-full ${STEP_DOT[step.status]} ${
                      step.status === 'running' ? 'animate-pulse' : ''
                    }`}
                  />
                </td>
                <td className="py-1.5 pr-3 font-mono">{step.id}</td>
                <td className="py-1.5 pr-3">{step.status}</td>
                <td className="py-1.5">{iterationSummary(step)}</td>
              </tr>
            ))}
        </tbody>
      </table>
    </div>
  )
}

// One line per iteration: which track passed at which tolerance.
function iterationSummary(step: ProgressStep): string {
  if (!step.id.startsWith('iteration-') || step.status !== 'succeeded') {
    return step.error ?? ''
  }
  const value = step.value as IcMuCalibrationIteration
  const track = (name: string, passed: boolean) => `${name} ${passed ? 'passed' : 'corrected'}`
  return `tolerance ${value.tolerance} · ${track('master', value.master.passed)} · ${track(
    'nonius',
    value.nonius.passed,
  )} · ${value.recordedRevolutions.toFixed(2)} ring turns`
}

interface Curve {
  label: string
  color: string
  points: IcMuCalibrationCurve
  /** The margin at the worst point, marked on the curve. Only the validation has one. */
  worst?: number
}

// Checked with the dataviz palette validator on the light surface: ocean and orange pass every check
// (CVD ΔE 14.7, normal-vision ΔE 29.5). Iteration 1 is context, so it stays neutral. Red is kept for
// the permissible error and used for nothing else.
const CURVE_COLOR = { first: '#a9a9a9', latest: '#00849b', validation: '#eb6834' } as const
const LIMIT_COLOR = '#8c072f'

const PLOT = { width: 760, height: 280, left: 48, right: 16, top: 12, bottom: 28 } as const

// The value nearest @p angle on a curve, for the hover readout.
function nearest(points: IcMuCalibrationCurve, angle: number): number[] | undefined {
  let best: number[] | undefined
  for (const point of points) {
    if (!best || Math.abs(point[0] - angle) < Math.abs(best[0] - angle)) {
      best = point
    }
  }
  return best
}

/**
 * The phase over one turn of the ring, with the permissible error as a band. The validation curve
 * is the one the run is judged by: its worst point sets the margin.
 */
function PhasePlot({ curves, limit }: { curves: Curve[]; limit: number }) {
  const [hover, setHover] = useState<number | null>(null)
  const plotWidth = PLOT.width - PLOT.left - PLOT.right
  const plotHeight = PLOT.height - PLOT.top - PLOT.bottom
  const extreme = Math.max(limit, ...curves.flatMap(c => c.points.map(p => Math.abs(p[1]))))
  const range = Math.ceil((extreme * 1.1) / 64) * 64
  const x = (angle: number) => PLOT.left + (angle / 360) * plotWidth
  const y = (value: number) => PLOT.top + plotHeight / 2 - (value / range) * (plotHeight / 2)
  const valueTicks = [-range, -limit, 0, limit, range].filter(
    (v, i, all) => all.indexOf(v) === i && Math.abs(v) <= range,
  )
  const path = (points: IcMuCalibrationCurve) =>
    [...points]
      .sort((a, b) => a[0] - b[0])
      .map(([angle, value], i) => `${i === 0 ? 'M' : 'L'}${x(angle).toFixed(1)},${y(value).toFixed(1)}`)
      .join('')
  const validation = curves.find(c => c.worst !== undefined)
  const worstPoint = validation?.points.reduce(
    (worst, p) => (Math.abs(p[1]) > Math.abs(worst[1]) ? p : worst),
    validation.points[0],
  )

  return (
    <figure className="max-w-4xl">
      <div className="flex flex-wrap gap-x-5 gap-y-1 mb-2 text-xs text-grey-800">
        {curves.map(curve => (
          <span key={curve.label} className="inline-flex items-center gap-1.5">
            <span className="inline-block h-0.5 w-4" style={{ background: curve.color }} />
            {curve.label}
            {curve.worst !== undefined && ' · the marked point sets the margin'}
          </span>
        ))}
        <span className="inline-flex items-center gap-1.5">
          <span className="inline-block h-0.5 w-4" style={{ background: LIMIT_COLOR }} />
          Permissible ±{limit}
        </span>
      </div>
      <svg
        viewBox={`0 0 ${PLOT.width} ${PLOT.height}`}
        className="w-full bg-white border border-grey-200 cursor-crosshair"
        onMouseMove={event => {
          const box = event.currentTarget.getBoundingClientRect()
          const svgX = ((event.clientX - box.left) / box.width) * PLOT.width
          const angle = ((svgX - PLOT.left) / plotWidth) * 360
          setHover(angle >= 0 && angle <= 360 ? angle : null)
        }}
        onMouseLeave={() => setHover(null)}
      >
        {/* Beyond the permissible error, a light wash; inside it, the plain surface. */}
        <rect x={PLOT.left} y={PLOT.top} width={plotWidth} height={y(limit) - PLOT.top} fill={LIMIT_COLOR} opacity={0.1} />
        <rect x={PLOT.left} y={y(-limit)} width={plotWidth} height={PLOT.top + plotHeight - y(-limit)} fill={LIMIT_COLOR} opacity={0.1} />
        {[0, 90, 180, 270, 360].map(angle => (
          <g key={angle}>
            <line x1={x(angle)} x2={x(angle)} y1={PLOT.top} y2={PLOT.top + plotHeight} stroke="#eeeeee" strokeWidth={1} />
            <text x={x(angle)} y={PLOT.height - 8} textAnchor="middle" className="fill-grey-700 text-[11px]">
              {angle}°
            </text>
          </g>
        ))}
        {valueTicks.map(value => (
          <g key={value}>
            <line
              x1={PLOT.left}
              x2={PLOT.left + plotWidth}
              y1={y(value)}
              y2={y(value)}
              stroke={Math.abs(value) === limit ? LIMIT_COLOR : '#eeeeee'}
              strokeWidth={1}
            />
            <text x={PLOT.left - 6} y={y(value) + 4} textAnchor="end" className="fill-grey-700 text-[11px]">
              {value}
            </text>
          </g>
        ))}
        {curves.map(curve => (
          <path
            key={curve.label}
            d={path(curve.points)}
            fill="none"
            stroke={curve.color}
            strokeWidth={curve.worst !== undefined ? 2 : 1.5}
            strokeLinejoin="round"
            strokeLinecap="round"
          />
        ))}
        {validation && worstPoint && (
          // The margin is the headroom between the worst point and the limit on its side, so the
          // bracket draws exactly that distance.
          (() => {
            const px = x(worstPoint[0])
            const py = y(worstPoint[1])
            const ly = y(worstPoint[1] >= 0 ? limit : -limit)
            const midY = (py + ly) / 2
            const left = worstPoint[0] > 300
            const label = `${validation.worst?.toFixed(1)} % margin`
            const labelWidth = label.length * 6.4 + 12
            const labelX = left ? px - 10 - labelWidth : px + 10
            return (
              <g>
                <line x1={px} x2={px} y1={py} y2={ly} stroke="#191817" strokeWidth={1.5} />
                <line x1={px - 5} x2={px + 5} y1={ly} y2={ly} stroke="#191817" strokeWidth={1.5} />
                <circle cx={px} cy={py} r={9} fill="#ffffff" opacity={0.85} />
                <circle cx={px} cy={py} r={6} fill={validation.color} stroke="#191817" strokeWidth={2} />
                <rect
                  x={labelX}
                  y={midY - 10}
                  width={labelWidth}
                  height={20}
                  rx={2}
                  fill="#ffffff"
                  stroke="#191817"
                  strokeWidth={1}
                />
                <text x={labelX + 6} y={midY + 4} className="fill-grey-900 text-[11px] font-semibold">
                  {label}
                </text>
              </g>
            )
          })()
        )}
        {hover !== null && (
          <line x1={x(hover)} x2={x(hover)} y1={PLOT.top} y2={PLOT.top + plotHeight} stroke="#656565" strokeWidth={1} />
        )}
      </svg>
      <div className="h-5 mt-1 text-xs text-grey-700">
        {hover !== null &&
          `${hover.toFixed(1)}° · ` +
            curves
              .map(curve => `${curve.label} ${nearest(curve.points, hover)?.[1] ?? '—'}`)
              .join(' · ')}
      </div>
      <figcaption className="text-xs text-grey-600">
        Phase over one turn of the ring, in the unit of the permissible error. The iterations show the
        phase error MU_3SL reports for each recording; the validation shows the phase margin, and its
        worst point sets the margin of the run.
      </figcaption>
    </figure>
  )
}
