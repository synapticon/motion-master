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
  const curves: { label: string; className: string; points: IcMuCalibrationCurve }[] = []
  if (done.length > 0) {
    curves.push({ label: 'Iteration 1', className: 'text-grey-400', points: done[0].phaseError })
  }
  if (done.length > 1) {
    const last = done[done.length - 1]
    curves.push({ label: `Iteration ${last.iteration}`, className: 'text-ocean', points: last.phaseError })
  }
  if (validation) {
    curves.push({ label: 'Validation margin', className: 'text-syn-red', points: validation.phaseMargin })
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

/** The phase curves over one ring turn, with the permissible error as dashed lines. */
function PhasePlot({
  curves,
  limit,
}: {
  curves: { label: string; className: string; points: IcMuCalibrationCurve }[]
  limit: number
}) {
  const width = 720
  const height = 240
  const range = Math.max(limit * 1.2, ...curves.flatMap(c => c.points.map(p => Math.abs(p[1]))))
  const x = (angle: number) => (angle / 360) * width
  const y = (value: number) => height / 2 - (value / range) * (height / 2)
  return (
    <figure>
      <svg viewBox={`0 0 ${width} ${height}`} className="w-full max-w-3xl border border-grey-200 bg-white">
        <line x1={0} x2={width} y1={y(0)} y2={y(0)} className="stroke-grey-200" />
        {[limit, -limit].map(l => (
          <line key={l} x1={0} x2={width} y1={y(l)} y2={y(l)} className="stroke-syn-red" strokeDasharray="4 4" />
        ))}
        {curves.map(curve => (
          <g key={curve.label} className={curve.className}>
            {[...curve.points]
              .sort((a, b) => a[0] - b[0])
              .map(([angle, value], i) => (
                <circle key={i} cx={x(angle)} cy={y(value)} r={1.2} className="fill-current" />
              ))}
          </g>
        ))}
      </svg>
      <figcaption className="text-xs text-grey-600 mt-1">
        Phase over the ring angle, 0° to 360°. Dashed: the permissible phase error.{' '}
        {curves.map(c => c.label).join(', ')}.
      </figcaption>
    </figure>
  )
}
