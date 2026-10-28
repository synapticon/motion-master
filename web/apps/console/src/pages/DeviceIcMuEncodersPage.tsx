import { useState } from 'react'
import { useParams } from 'react-router'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import {
  apiErrorMessage,
  type EncoderConfiguration,
  type ProcedureSnapshot,
  type ProgressStep,
} from '@synapticon/motion-master-client'
import Callout from '../components/Callout'
import DevicePageHeader from '../components/DevicePageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { isIcMuEncoder } from '../utils/icMuEncoder'
import { btnOutline, btnPrimary } from '../utils/styles'

const STATUS_PROCEDURE = 'ic-mu-encoder-status'
const CONFIGURATION_PROCEDURE = 'ic-mu-encoder-configuration'

const inputCls = 'border border-grey-300 px-3 h-[38px] text-sm bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'

// The status of one encoder, as the status procedure reports it in its read-registers step and the
// configuration in its verify step.
interface IcMuEncoderStatus {
  encoderOrdinal: 1 | 2
  sensorPort: number
  singleturnBits: number
  multiturnBits: number
  masterPolePairs: number | null
  hardRev: number
  part: string | null
  revision: number
  outputRegister: number
  expectedOutputRegister: number | null
  state: 'not-configured' | 'invalid' | 'configured' | null
  reason: string | null
  path: 'singleturn' | 'multiturn'
  batteryModes: { name: string; maxCurrentMicroamps: number; maxAccelerationKrpmPerSecond: number }[]
}

const STEP_DOT: Record<ProgressStep['status'], string> = {
  idle: 'bg-grey-300',
  running: 'bg-ocean',
  succeeded: 'bg-green-600',
  failed: 'bg-syn-red',
}

const STATE_BADGE: Record<NonNullable<IcMuEncoderStatus['state']>, string> = {
  configured: 'bg-green-600 text-white',
  invalid: 'bg-status-warn text-grey-900',
  'not-configured': 'bg-grey-700 text-white',
}

function hex(n: number): string {
  return `0x${n.toString(16).toUpperCase().padStart(2, '0')}`
}

function stepValue(snapshot: ProcedureSnapshot | undefined, id: string): unknown {
  return snapshot?.steps.find(step => step.id === id)?.value
}

export default function DeviceIcMuEncodersPage() {
  const { deviceId } = useParams()
  const slavePosition = Number(deviceId)
  const { api } = useConnection()
  const queryClient = useQueryClient()

  const encodersQuery = useQuery({
    queryKey: ['encoders', slavePosition],
    queryFn: () => api.getEncoders(slavePosition).then(r => r.data),
    retry: false,
  })

  // The last snapshot of each procedure. Reading one runs nothing on the drive, so opening the page
  // never takes the busy token from another procedure.
  const snapshotQuery = (name: string) => ({
    queryKey: ['procedure', slavePosition, name],
    queryFn: () => api.getProcedure(slavePosition, name).then(r => r.data),
    refetchInterval: (query: { state: { data?: ProcedureSnapshot } }) =>
      query.state.data?.status === 'running' ? 300 : false,
    refetchIntervalInBackground: true,
    retry: false,
  })
  const statusQuery = useQuery(snapshotQuery(STATUS_PROCEDURE))
  const configurationQuery = useQuery(snapshotQuery(CONFIGURATION_PROCEDURE))

  const start = useMutation({
    mutationFn: ({ name, body }: { name: string; body: Record<string, unknown> }) =>
      api.startProcedure(slavePosition, name, body),
    onSettled: (_data, _error, { name }) =>
      queryClient.invalidateQueries({ queryKey: ['procedure', slavePosition, name] }),
  })

  const running =
    statusQuery.data?.status === 'running' || configurationQuery.data?.status === 'running'
  const statuses = (stepValue(statusQuery.data, 'read-registers') ?? []) as IcMuEncoderStatus[]
  const encoders = (encodersQuery.data?.encoders ?? []).filter(isIcMuEncoder)
  const configured = stepValue(configurationQuery.data, 'verify') as IcMuEncoderStatus | undefined

  return (
    <div>
      <DevicePageHeader
        slavePosition={slavePosition}
        title="iC-MU Encoders"
        description="Whether each iC-MU encoder of this drive holds the configuration its encoder object asks for, and a way to configure it."
      />

      <div className="p-4 sm:px-8 sm:py-7 space-y-6 max-w-5xl">
        <Callout variant="info">
          The drive does not configure its iC-MU encoders. The configuration writes the registers of
          the encoder with OS command 0 and copies them into the EEPROM of the encoder. The values
          come from the singleturn and multiturn bits of the encoder object, 0x2110 or 0x2112. On
          sensor port 1, an encoder with multiturn bits also gets the iC-PVL beside it configured.
          The configuration ends with the restore file the drive uses when the EEPROM of the iC-MU
          is corrupted. Sensor ports 3 to 5 take an external encoder: it is treated as an iC-MU only
          when register 0x74 names an iC-MU part, always takes the singleturn path, and gets no
          restore file, because the drive keeps one only for ports 1 and 2.
        </Callout>

        <div className="flex flex-wrap items-center gap-3">
          <button
            type="button"
            className={btnOutline}
            disabled={running || start.isPending}
            onClick={() => start.mutate({ name: STATUS_PROCEDURE, body: {} })}
          >
            Read status
          </button>
          <span className="text-xs text-grey-600">
            {statusQuery.data?.finishedAt
              ? `Last read ${new Date(statusQuery.data.finishedAt).toLocaleTimeString()}`
              : 'Not read yet. Reading runs OS command 0 and writes nothing.'}
          </span>
        </div>

        {start.isError && <Callout variant="error">{apiErrorMessage(start.error)}</Callout>}
        {statusQuery.data?.status === 'failed' && (
          <Callout variant="error">
            {statusQuery.data.steps.find(step => step.status === 'failed')?.error ??
              'The status read failed.'}
          </Callout>
        )}
        {encodersQuery.isError && (
          <Callout variant="error">{apiErrorMessage(encodersQuery.error)}</Callout>
        )}
        {encodersQuery.isSuccess && encoders.length === 0 && (
          <Callout variant="info">
            This drive has no BiSS or internal encoder with singleturn bits on a sensor port.
          </Callout>
        )}

        {encoders.map(encoder => (
          <EncoderCard
            key={encoder.encoderOrdinal}
            encoder={encoder}
            status={
              configured?.encoderOrdinal === encoder.encoderOrdinal &&
              (configurationQuery.data?.finishedAt ?? 0) > (statusQuery.data?.finishedAt ?? 0)
                ? configured
                : statuses.find(s => s.encoderOrdinal === encoder.encoderOrdinal)
            }
            busy={running || start.isPending}
            onConfigure={batteryMode =>
              start.mutate({
                name: CONFIGURATION_PROCEDURE,
                body: { encoderOrdinal: encoder.encoderOrdinal, ...(batteryMode ? { batteryMode } : {}) },
              })
            }
          />
        ))}

        {configurationQuery.data && configurationQuery.data.status !== 'idle' && (
          <ConfigurationRun snapshot={configurationQuery.data} />
        )}
      </div>
    </div>
  )
}

function EncoderCard({
  encoder,
  status,
  busy,
  onConfigure,
}: {
  encoder: EncoderConfiguration
  status: IcMuEncoderStatus | undefined
  busy: boolean
  onConfigure: (batteryMode: string | null) => void
}) {
  const [batteryMode, setBatteryMode] = useState('default')
  const multiturn = status?.path === 'multiturn'

  return (
    <section className="border border-grey-200 bg-white">
      <div className="flex flex-wrap items-center justify-between gap-3 px-5 py-3 border-b border-grey-200">
        <h3 className="font-display uppercase tracking-wide text-sm">
          Encoder {encoder.encoderOrdinal} · sensor port {encoder.sensorPort}
          {encoder.sensorPort >= 3 ? ' · external' : ''}
        </h3>
        {status?.state ? (
          <span className={`px-2 py-0.5 text-xs ${STATE_BADGE[status.state]}`}>{status.state}</span>
        ) : (
          <span className="text-xs text-grey-500">{status ? 'no state' : 'status not read'}</span>
        )}
      </div>

      <dl className="grid grid-cols-2 sm:grid-cols-4 gap-x-6 gap-y-3 px-5 py-4 text-sm">
        <Field label="Singleturn bits" value={encoder.singleturnBits} />
        <Field label="Multiturn bits" value={encoder.multiturnBits} />
        <Field label="Master pole pairs" value={status?.masterPolePairs ?? '—'} />
        <Field label="Path" value={status?.path ?? '—'} />
        <Field
          label="Part"
          value={status ? `${status.part ?? 'unknown part'} · rev ${status.revision}` : '—'}
        />
        <Field label="HARD_REV (0x74)" value={status ? hex(status.hardRev) : '—'} />
        <Field label="0x11 read" value={status ? hex(status.outputRegister) : '—'} />
        <Field
          label="0x11 expected"
          value={status?.expectedOutputRegister != null ? hex(status.expectedOutputRegister) : '—'}
        />
      </dl>
      {status?.reason && (
        <p className="px-5 pb-3 text-xs text-status-bad">{status.reason}</p>
      )}

      <div className="flex flex-wrap items-end gap-3 px-5 py-4 border-t border-grey-100">
        {multiturn && (
          <div>
            <label className={labelCls} htmlFor={`battery-${encoder.encoderOrdinal}`}>
              Battery mode
            </label>
            <select
              id={`battery-${encoder.encoderOrdinal}`}
              className={inputCls}
              value={batteryMode}
              onChange={event => setBatteryMode(event.target.value)}
            >
              <option value="default">Default, from the shaft the encoder measures</option>
              {status.batteryModes.map(mode => (
                <option key={mode.name} value={mode.name}>
                  {mode.maxCurrentMicroamps} µA, up to {mode.maxAccelerationKrpmPerSecond} krpm/s
                </option>
              ))}
            </select>
          </div>
        )}
        <button
          type="button"
          className={`${btnPrimary} h-[38px]`}
          disabled={busy || !status?.expectedOutputRegister}
          onClick={() => onConfigure(multiturn && batteryMode !== 'default' ? batteryMode : null)}
        >
          Configure
        </button>
        <p className="text-xs text-grey-600 max-w-xl">
          Writes the EEPROM of the encoder. The calibration registers keep their values. The drive
          must not be in Operation Enabled, and the encoder reports no position while it is
          configured. Read the status first.
        </p>
      </div>
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

function ConfigurationRun({ snapshot }: { snapshot: ProcedureSnapshot }) {
  const validate = stepValue(snapshot, 'validate') as { encoderOrdinal?: number } | undefined
  return (
    <section className="border border-grey-200 bg-white">
      <div className="px-5 py-3 border-b border-grey-200 flex items-center justify-between">
        <h3 className="font-display uppercase tracking-wide text-sm">
          Last configuration{validate?.encoderOrdinal ? ` · encoder ${validate.encoderOrdinal}` : ''}
        </h3>
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
    </section>
  )
}
