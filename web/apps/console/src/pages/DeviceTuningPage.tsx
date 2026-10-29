import { useEffect, useState } from 'react'
import { Link, useParams } from 'react-router'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import {
  apiErrorMessage,
  type ControllerTuningComputed,
  type ControllerTuningWrite,
  type ProcedureSnapshot,
  type ProgressStep,
} from '@synapticon/motion-master-client'
import Callout from '../components/Callout'
import Checkbox from '../components/Checkbox'
import DevicePageHeader from '../components/DevicePageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline, btnPrimary } from '../utils/styles'

const PROCEDURE = 'controller-auto-tuning'
const LIMITS_PROCEDURE = 'set-integral-limits'

const inputCls = 'border border-grey-300 px-3 h-[38px] text-sm bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'

const STEP_DOT: Record<ProgressStep['status'], string> = {
  idle: 'bg-grey-300',
  running: 'bg-ocean',
  succeeded: 'bg-green-600',
  failed: 'bg-syn-red',
}

type Controller = 'position' | 'velocity'

interface GainObject {
  index: number
  subindex: number
  label: string
}

// The objects the tuning writes, in the order it writes them.
const GAINS: Record<Controller, GainObject[]> = {
  position: [
    { index: 0x2012, subindex: 0x01, label: 'Position loop Kp' },
    { index: 0x2012, subindex: 0x02, label: 'Position loop Ki' },
    { index: 0x2012, subindex: 0x03, label: 'Position loop Kd' },
    { index: 0x2012, subindex: 0x05, label: 'Velocity loop Kp' },
    { index: 0x2012, subindex: 0x06, label: 'Velocity loop Ki' },
    { index: 0x2012, subindex: 0x07, label: 'Velocity loop Kd' },
  ],
  velocity: [
    { index: 0x2011, subindex: 0x01, label: 'Kp' },
    { index: 0x2011, subindex: 0x02, label: 'Ki' },
    { index: 0x2011, subindex: 0x03, label: 'Kd' },
  ],
}

// The limits set-integral-limits writes, in the order it writes them.
const LIMITS: GainObject[] = [
  { index: 0x2011, subindex: 0x04, label: 'Velocity controller' },
  { index: 0x2012, subindex: 0x04, label: 'Position loop' },
  { index: 0x2012, subindex: 0x08, label: 'Velocity loop of the position controller' },
]

interface LimitsWrite {
  limits: { object: string; before: number; after: number }[]
  skipped: string[]
}

function objectName(index: number, subindex: number): string {
  return `0x${index.toString(16).toUpperCase()}:${subindex.toString(16).toUpperCase().padStart(2, '0')}`
}

function stepValue<T>(snapshot: ProcedureSnapshot | undefined, id: string): T | undefined {
  return snapshot?.steps.find(step => step.id === id)?.value as T | undefined
}

function isNotFound(error: unknown): boolean {
  return typeof error === 'object' && error !== null && (error as { status?: unknown }).status === 404
}

function formatNumber(value: unknown): string {
  if (typeof value !== 'number') {
    return value == null ? '—' : String(value)
  }
  return Number.isInteger(value) ? String(value) : value.toPrecision(6)
}

export default function DeviceTuningPage() {
  const { deviceId } = useParams()
  const slavePosition = Number(deviceId)
  const { api } = useConnection()
  const queryClient = useQueryClient()

  const [controller, setController] = useState<Controller>('position')
  const [controllerType, setControllerType] = useState<'PI-P' | 'P-PI'>('PI-P')
  const [full, setFull] = useState(true)
  const [dampingRatioPermil, setDampingRatioPermil] = useState(1000)
  const [settlingTimeMs, setSettlingTimeMs] = useState(200)
  const [bandwidthMilliHz, setBandwidthMilliHz] = useState(5000)

  const plantModelQuery = useQuery({
    queryKey: ['plant-model', slavePosition],
    queryFn: () => api.getDevicePlantModel(slavePosition).then(r => r.data),
    retry: false,
  })

  // Each gain object is read on its own, because a drive may lack a Kd object and that is an
  // answer, not a failure of the others.
  const gainsQuery = useQuery({
    queryKey: ['controller-gains', slavePosition],
    queryFn: async () => {
      const objects = [...GAINS.position, ...GAINS.velocity, ...LIMITS]
      const results = await Promise.allSettled(
        objects.map(o => api.readParameter(slavePosition, o.index, o.subindex).then(r => r.data)),
      )
      const values = new Map<string, number | 'missing' | string>()
      results.forEach((result, i) => {
        const key = objectName(objects[i].index, objects[i].subindex)
        if (result.status === 'fulfilled') {
          values.set(key, result.value.value as number)
        } else {
          values.set(key, isNotFound(result.reason) ? 'missing' : apiErrorMessage(result.reason))
        }
      })
      return values
    },
    retry: false,
  })

  const snapshotQuery = useQuery({
    queryKey: ['procedure', slavePosition, PROCEDURE],
    queryFn: () => api.getProcedure(slavePosition, PROCEDURE).then(r => r.data),
    refetchInterval: query => (query.state.data?.status === 'running' ? 300 : false),
    refetchIntervalInBackground: true,
    retry: false,
  })

  const start = useMutation({
    mutationFn: () =>
      api.startProcedure(slavePosition, PROCEDURE, {
        controller,
        full,
        ...(controller === 'position' ? { controllerType } : {}),
        ...(full
          ? {}
          : {
              dampingRatioPermil,
              ...(controller === 'position' ? { settlingTimeMs } : { bandwidthMilliHz }),
            }),
      }),
    onSettled: () =>
      queryClient.invalidateQueries({ queryKey: ['procedure', slavePosition, PROCEDURE] }),
  })

  const limitsQuery = useQuery({
    queryKey: ['procedure', slavePosition, LIMITS_PROCEDURE],
    queryFn: () => api.getProcedure(slavePosition, LIMITS_PROCEDURE).then(r => r.data),
    refetchInterval: query => (query.state.data?.status === 'running' ? 300 : false),
    refetchIntervalInBackground: true,
    retry: false,
  })
  const setLimits = useMutation({
    mutationFn: () => api.startProcedure(slavePosition, LIMITS_PROCEDURE, {}),
    onSettled: () =>
      queryClient.invalidateQueries({ queryKey: ['procedure', slavePosition, LIMITS_PROCEDURE] }),
  })

  const snapshot = snapshotQuery.data
  const limitsSnapshot = limitsQuery.data
  const running =
    snapshot?.status === 'running' ||
    start.isPending ||
    limitsSnapshot?.status === 'running' ||
    setLimits.isPending
  // The gains change at the end of a run, so they are read again once it has finished.
  const finishedAt = snapshot?.finishedAt
  const limitsFinishedAt = limitsSnapshot?.finishedAt
  useEffect(() => {
    if (finishedAt || limitsFinishedAt) {
      void queryClient.invalidateQueries({ queryKey: ['controller-gains', slavePosition] })
    }
  }, [finishedAt, limitsFinishedAt, queryClient, slavePosition])

  const noPlantModel = isNotFound(plantModelQuery.error)
  const elapsedSeconds =
    snapshot?.startedAt && snapshot.finishedAt
      ? ((snapshot.finishedAt - snapshot.startedAt) / 1000).toFixed(1)
      : undefined

  return (
    <div>
      <DevicePageHeader
        slavePosition={slavePosition}
        title="Tuning"
        description="Computes the gains of the position or the velocity controller from the plant model of the drive, and sets the integral limits of the controllers."
      />

      <div className="p-4 sm:px-8 sm:py-7 space-y-6 max-w-5xl">
        <Callout variant="info">
          The auto-tuning program computes the gains from the plant model in plant_model.csv, which
          plant identification writes. A full tuning also reads the drive configuration and records
          the velocity noise of the motor at rest for 500 ms, and the program chooses the damping
          ratio and the settling time or the bandwidth. Otherwise the gains are computed for the
          values you give. The drive uses the new gains on its next control cycle, in any state.
          Nothing moves the motor, and nothing is stored to flash: run store-parameters on the
          Procedures page to keep the gains after a restart.
        </Callout>

        <section className="border border-grey-200 bg-white">
          <div className="px-5 py-3 border-b border-grey-200 flex items-center justify-between">
            <h3 className="font-display uppercase tracking-wide text-sm">Plant model</h3>
            <Link
              to={`/devices/${slavePosition}/procedures/plant-identification`}
              className="text-xs underline"
            >
              Plant identification
            </Link>
          </div>
          {plantModelQuery.isPending && <p className="px-5 py-4 text-sm text-grey-600">Reading…</p>}
          {noPlantModel && (
            <p className="px-5 py-4 text-sm">
              The drive has no plant_model.csv. Run plant identification first.
            </p>
          )}
          {plantModelQuery.isError && !noPlantModel && (
            <p className="px-5 py-4 text-sm text-status-bad">
              {apiErrorMessage(plantModelQuery.error)}
            </p>
          )}
          {plantModelQuery.data && (
            <dl className="grid grid-cols-2 sm:grid-cols-4 gap-x-6 gap-y-3 px-5 py-4 text-sm">
              <Field
                label="Numerator"
                value={plantModelQuery.data.numerator.map(formatNumber).join(', ')}
              />
              <Field
                label="Denominator"
                value={plantModelQuery.data.denominator.map(formatNumber).join(', ')}
              />
              <Field label="Inertia" value={formatNumber(plantModelQuery.data.inertia)} />
              <Field label="Friction" value={formatNumber(plantModelQuery.data.friction)} />
              <Field
                label="Chirp"
                value={`${plantModelQuery.data.chirp.startFrequencyHz}–${plantModelQuery.data.chirp.endFrequencyHz} Hz`}
              />
              <Field
                label="Amplitude"
                value={`${plantModelQuery.data.chirp.amplitudePermil} ‰`}
              />
              <Field label="Duration" value={`${plantModelQuery.data.chirp.durationMs} ms`} />
              <Field label="Signal" value={plantModelQuery.data.chirp.signalType} />
            </dl>
          )}
        </section>

        <section className="border border-grey-200 bg-white">
          <div className="px-5 py-3 border-b border-grey-200">
            <h3 className="font-display uppercase tracking-wide text-sm">Auto-tune</h3>
          </div>
          <div className="flex flex-wrap items-end gap-4 px-5 py-4">
            <div>
              <label className={labelCls} htmlFor="tuning-controller">
                Controller
              </label>
              <select
                id="tuning-controller"
                className={inputCls}
                value={controller}
                onChange={event => setController(event.target.value as Controller)}
              >
                <option value="position">Position</option>
                <option value="velocity">Velocity</option>
              </select>
            </div>
            {controller === 'position' && (
              <div>
                <label className={labelCls} htmlFor="tuning-type">
                  Cascaded controller type
                </label>
                <select
                  id="tuning-type"
                  className={inputCls}
                  value={controllerType}
                  onChange={event => setControllerType(event.target.value as 'PI-P' | 'P-PI')}
                >
                  <option value="PI-P">PI-P</option>
                  <option value="P-PI">P-PI</option>
                </select>
              </div>
            )}
            <label className="flex items-center gap-2 h-[38px] text-sm">
              <Checkbox checked={full} onChange={setFull} />
              Full auto-tuning
            </label>
          </div>
          {!full && (
            <div className="flex flex-wrap items-end gap-4 px-5 pb-4">
              <NumberField
                id="tuning-damping"
                label="Damping ratio (‰)"
                value={dampingRatioPermil}
                min={400}
                max={2000}
                step={100}
                onChange={setDampingRatioPermil}
              />
              {controller === 'position' ? (
                <NumberField
                  id="tuning-settling"
                  label="Settling time (ms)"
                  value={settlingTimeMs}
                  min={20}
                  max={1000}
                  step={10}
                  onChange={setSettlingTimeMs}
                />
              ) : (
                <NumberField
                  id="tuning-bandwidth"
                  label="Bandwidth (mHz)"
                  value={bandwidthMilliHz}
                  min={200}
                  max={100000}
                  step={200}
                  onChange={setBandwidthMilliHz}
                />
              )}
            </div>
          )}
          <p className="px-5 pb-3 text-xs text-grey-600">
            {full
              ? 'PI-P puts the integral action in the position loop: a faster response with overshoot. P-PI puts it in the velocity loop: a damped response.'
              : 'A damping ratio above 1000 ‰ removes overshoot and settles more slowly. Below 1000 ‰ responds faster with overshoot.'}
          </p>
          <div className="flex flex-wrap items-center gap-3 px-5 py-4 border-t border-grey-100">
            <button
              type="button"
              className={btnPrimary}
              disabled={running || noPlantModel}
              onClick={() => start.mutate()}
            >
              {running ? 'Tuning…' : 'Tune and write the gains'}
            </button>
            <span className="text-xs text-grey-600">
              {elapsedSeconds ? `Last run took ${elapsedSeconds} s` : 'Not run yet'}
            </span>
          </div>
        </section>

        {start.isError && <Callout variant="error">{apiErrorMessage(start.error)}</Callout>}

        <GainsCard
          values={gainsQuery.data}
          error={gainsQuery.isError ? apiErrorMessage(gainsQuery.error) : undefined}
          written={stepValue<ControllerTuningWrite>(snapshot, 'write')}
          onRead={() =>
            void queryClient.invalidateQueries({ queryKey: ['controller-gains', slavePosition] })
          }
          busy={gainsQuery.isFetching}
        />

        {snapshot && snapshot.status !== 'idle' && <TuningRun snapshot={snapshot} />}

        <LimitsCard
          values={gainsQuery.data}
          snapshot={limitsSnapshot}
          busy={running}
          onSet={() => setLimits.mutate()}
          error={setLimits.isError ? apiErrorMessage(setLimits.error) : undefined}
        />
      </div>
    </div>
  )
}

function NumberField({
  id,
  label,
  value,
  min,
  max,
  step,
  onChange,
}: {
  id: string
  label: string
  value: number
  min: number
  max: number
  step: number
  onChange: (value: number) => void
}) {
  return (
    <div>
      <label className={labelCls} htmlFor={id}>
        {label}
      </label>
      <input
        id={id}
        type="number"
        className={`${inputCls} w-36`}
        value={value}
        min={min}
        max={max}
        step={step}
        onChange={event => onChange(Number(event.target.value))}
      />
    </div>
  )
}

function GainsCard({
  values,
  error,
  written,
  onRead,
  busy,
}: {
  values: Map<string, number | string> | undefined
  error: string | undefined
  written: ControllerTuningWrite | undefined
  onRead: () => void
  busy: boolean
}) {
  const before = new Map((written?.gains ?? []).map(gain => [gain.object, gain.before]))
  return (
    <section className="border border-grey-200 bg-white">
      <div className="px-5 py-3 border-b border-grey-200 flex items-center justify-between">
        <h3 className="font-display uppercase tracking-wide text-sm">Controller gains</h3>
        <button type="button" className={btnOutline} disabled={busy} onClick={onRead}>
          Read
        </button>
      </div>
      {error && <p className="px-5 py-3 text-sm text-status-bad">{error}</p>}
      <div className="grid sm:grid-cols-2 gap-6 px-5 py-4">
        {(['position', 'velocity'] as const).map(controller => (
          <table key={controller} className="w-full text-sm">
            <caption className="text-left text-xs text-grey-600 uppercase tracking-wide pb-2">
              {controller === 'position' ? 'Position controller' : 'Velocity controller'}
            </caption>
            <thead>
              <tr className="text-xs text-grey-600">
                <th className="text-left font-normal py-1">Object</th>
                <th className="text-left font-normal py-1">Gain</th>
                <th className="text-right font-normal py-1">Value</th>
                <th className="text-right font-normal py-1">Before the last run</th>
              </tr>
            </thead>
            <tbody>
              {GAINS[controller].map(gain => {
                const name = objectName(gain.index, gain.subindex)
                const value = values?.get(name)
                return (
                  <tr key={name} className="border-t border-grey-100">
                    <td className="py-1.5 font-mono text-xs">{name}</td>
                    <td className="py-1.5">{gain.label}</td>
                    <td className="py-1.5 text-right font-mono">
                      {value === 'missing' ? (
                        <span className="text-grey-500">not in the dictionary</span>
                      ) : typeof value === 'string' ? (
                        <span className="text-status-bad text-xs">{value}</span>
                      ) : (
                        formatNumber(value)
                      )}
                    </td>
                    <td className="py-1.5 text-right font-mono text-grey-600">
                      {before.has(name) ? formatNumber(before.get(name)) : ''}
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        ))}
      </div>
    </section>
  )
}

function LimitsCard({
  values,
  snapshot,
  busy,
  onSet,
  error,
}: {
  values: Map<string, number | string> | undefined
  snapshot: ProcedureSnapshot | undefined
  busy: boolean
  onSet: () => void
  error: string | undefined
}) {
  const written = stepValue<LimitsWrite>(snapshot, 'write')
  const before = new Map((written?.limits ?? []).map(limit => [limit.object, limit.before]))
  const failed = snapshot?.steps.find(step => step.status === 'failed')
  return (
    <section className="border border-grey-200 bg-white">
      <div className="px-5 py-3 border-b border-grey-200">
        <h3 className="font-display uppercase tracking-wide text-sm">Integral limits</h3>
      </div>
      <p className="px-5 pt-4 text-xs text-grey-600">
        The velocity loops stop their integrator at the maximum torque, 0x6072 / 1000 × 0x6076, in
        mNm. The position loop of the cascaded controller stops at the maximum motor speed on the
        driving shaft, 0x6080 × feed constant / gear ratio, and the simple PID position loop at the
        maximum torque. 2147483647 means the limit is off.
      </p>
      <div className="px-5 py-4">
        <table className="w-full text-sm">
          <thead>
            <tr className="text-xs text-grey-600">
              <th className="text-left font-normal py-1">Object</th>
              <th className="text-left font-normal py-1">Limit</th>
              <th className="text-right font-normal py-1">Value</th>
              <th className="text-right font-normal py-1">Before the last run</th>
            </tr>
          </thead>
          <tbody>
            {LIMITS.map(limit => {
              const name = objectName(limit.index, limit.subindex)
              const value = values?.get(name)
              return (
                <tr key={name} className="border-t border-grey-100">
                  <td className="py-1.5 font-mono text-xs">{name}</td>
                  <td className="py-1.5">{limit.label}</td>
                  <td className="py-1.5 text-right font-mono">
                    {value === 'missing' ? (
                      <span className="text-grey-500">not in the dictionary</span>
                    ) : typeof value === 'string' ? (
                      <span className="text-status-bad text-xs">{value}</span>
                    ) : (
                      formatNumber(value)
                    )}
                  </td>
                  <td className="py-1.5 text-right font-mono text-grey-600">
                    {before.has(name) ? formatNumber(before.get(name)) : ''}
                  </td>
                </tr>
              )
            })}
          </tbody>
        </table>
        {written && written.skipped.length > 0 && (
          <p className="pt-2 text-xs text-grey-600">
            Not in the dictionary, so not written: {written.skipped.join(', ')}
          </p>
        )}
      </div>
      {(error ?? failed?.error) && (
        <div className="px-5 pb-4">
          <Callout variant="error">{error ?? failed?.error}</Callout>
        </div>
      )}
      <div className="flex flex-wrap items-center gap-3 px-5 py-4 border-t border-grey-100">
        <button type="button" className={btnPrimary} disabled={busy} onClick={onSet}>
          Set integral limits
        </button>
        <span className="text-xs text-grey-600">
          {snapshot?.finishedAt
            ? `Last set ${new Date(snapshot.finishedAt).toLocaleTimeString()}, ${snapshot.status}`
            : 'Not set yet. Nothing is stored to flash.'}
        </span>
      </div>
    </section>
  )
}

function TuningRun({ snapshot }: { snapshot: ProcedureSnapshot }) {
  const computed = stepValue<ControllerTuningComputed>(snapshot, 'compute')
  const written = stepValue<ControllerTuningWrite>(snapshot, 'write')
  const noise = stepValue<{ measured: boolean; noiseRpm?: number; warning?: string | null }>(
    snapshot,
    'noise',
  )
  const failed = snapshot.steps.find(step => step.status === 'failed')
  const state =
    computed?.state && typeof computed.state === 'object'
      ? Object.entries(computed.state as Record<string, unknown>).filter(
          ([, value]) => typeof value === 'number',
        )
      : []
  return (
    <section className="border border-grey-200 bg-white">
      <div className="px-5 py-3 border-b border-grey-200 flex items-center justify-between">
        <h3 className="font-display uppercase tracking-wide text-sm">Last run</h3>
        <span className="text-xs text-grey-600">{snapshot.status}</span>
      </div>
      <table className="w-full text-xs">
        <tbody>
          {snapshot.steps.map(step => (
            <tr key={step.id} className="border-b border-grey-100 last:border-0 align-top">
              <td className="pl-5 py-2 w-6">
                <span
                  className={`inline-block h-2 w-2 rounded-full ${STEP_DOT[step.status]} ${
                    step.status === 'running' ? 'animate-pulse' : ''
                  }`}
                />
              </td>
              <td className="px-3 py-2 font-mono">{step.id}</td>
              <td className="px-3 py-2">{step.status}</td>
              <td className="px-3 py-2">
                {step.error && <span className="text-status-bad">{step.error}</span>}
              </td>
            </tr>
          ))}
        </tbody>
      </table>

      {failed?.error && (
        <div className="px-5 py-3 border-t border-grey-100">
          <Callout variant="error">{failed.error}</Callout>
        </div>
      )}
      {written?.warning && (
        <div className="px-5 py-3 border-t border-grey-100">
          <Callout variant="warning">{written.warning}</Callout>
        </div>
      )}

      {computed && (
        <dl className="grid grid-cols-2 sm:grid-cols-4 gap-x-6 gap-y-3 px-5 py-4 text-sm border-t border-grey-100">
          <Field label="Tuning" value={computed.full ? 'full' : 'to targets'} />
          {computed.controllerType && <Field label="Type" value={computed.controllerType} />}
          <Field label="Damping ratio" value={formatNumber(computed.dampingRatio)} />
          {computed.settlingTimeMs != null && (
            <Field label="Settling time" value={`${formatNumber(computed.settlingTimeMs)} ms`} />
          )}
          {computed.bandwidthHz != null && (
            <Field label="Bandwidth" value={`${formatNumber(computed.bandwidthHz)} Hz`} />
          )}
          {noise?.measured && (
            <Field label="Velocity noise" value={`${formatNumber(noise.noiseRpm)} rpm`} />
          )}
          {state.map(([name, value]) => (
            <Field key={name} label={name} value={formatNumber(value)} />
          ))}
        </dl>
      )}

      {written && (
        <div className="px-5 py-4 border-t border-grey-100">
          <table className="w-full text-sm">
            <thead>
              <tr className="text-xs text-grey-600">
                <th className="text-left font-normal py-1">Object</th>
                <th className="text-right font-normal py-1">Before</th>
                <th className="text-right font-normal py-1">Written</th>
              </tr>
            </thead>
            <tbody>
              {written.gains.map(gain => (
                <tr key={gain.object} className="border-t border-grey-100">
                  <td className="py-1.5 font-mono text-xs">{gain.object}</td>
                  <td className="py-1.5 text-right font-mono">{formatNumber(gain.before)}</td>
                  <td className="py-1.5 text-right font-mono">{formatNumber(gain.after)}</td>
                </tr>
              ))}
            </tbody>
          </table>
          {written.skipped.length > 0 && (
            <p className="pt-2 text-xs text-grey-600">
              Not in the dictionary, so not written: {written.skipped.join(', ')}
            </p>
          )}
        </div>
      )}
    </section>
  )
}

function Field({ label, value }: { label: string; value: string | number }) {
  return (
    <div>
      <dt className={labelCls}>{label}</dt>
      <dd className="font-mono">{value}</dd>
    </div>
  )
}
