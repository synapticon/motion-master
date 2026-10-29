import { useEffect, useState } from 'react'
import { useParams } from 'react-router'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import {
  apiErrorMessage,
  type EncoderConfiguration,
  type IcMuEncoderStatus,
  type ProcedureSnapshot,
  type ProgressStep,
} from '@synapticon/motion-master-client'
import Callout from '../components/Callout'
import Checkbox from '../components/Checkbox'
import DevicePageHeader from '../components/DevicePageHeader'
import IcMuCalibrationSection from '../components/IcMuCalibrationSection'
import IcMuEepromSection from '../components/IcMuEepromSection'
import { useConnection } from '../contexts/ConnectionContext'
import { isIcMuEncoder } from '../utils/icMuEncoder'
import { btnOutline, btnPrimary } from '../utils/styles'

const STATUS_PROCEDURE = 'ic-mu-encoder-status'
const CONFIGURATION_PROCEDURE = 'ic-mu-encoder-configuration'

const inputCls = 'border border-grey-300 px-3 h-[38px] text-sm bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'

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
  // The calibration exists only on a server built with MU_3SL, so the page asks for it.
  const proceduresQuery = useQuery({
    queryKey: ['procedures', slavePosition],
    queryFn: () => api.listProcedures(slavePosition).then(r => r.data),
    retry: false,
  })
  const calibrationAvailable = (proceduresQuery.data ?? []).some(
    listing => listing.descriptor.name === 'ic-mu-calibration',
  )
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
            slavePosition={slavePosition}
            calibrationAvailable={calibrationAvailable}
            roles={(['position', 'velocity', 'commutation'] as const).filter(
              role => encodersQuery.data?.[role] === encoder.encoderOrdinal,
            )}
            onConfigure={(batteryMode, writeRestoreFile) =>
              start.mutate({
                name: CONFIGURATION_PROCEDURE,
                body: {
                  encoderOrdinal: encoder.encoderOrdinal,
                  writeRestoreFile,
                  ...(batteryMode ? { batteryMode } : {}),
                },
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
  slavePosition,
  calibrationAvailable,
  roles,
  onConfigure,
}: {
  encoder: EncoderConfiguration
  status: IcMuEncoderStatus | undefined
  busy: boolean
  slavePosition: number
  calibrationAvailable: boolean
  /** The controllers that use this encoder, as GET /encoders names them. */
  roles: readonly ('position' | 'velocity' | 'commutation')[]
  onConfigure: (batteryMode: string | null, writeRestoreFile: boolean) => void
}) {
  const [batteryMode, setBatteryMode] = useState('default')
  const multiturn = status?.path === 'multiturn'
  // The drive keeps a restore file only for sensor ports 1 and 2.
  const restoreFilePort = encoder.sensorPort === 1 || encoder.sensorPort === 2
  const [writeRestoreFile, setWriteRestoreFile] = useState(true)

  return (
    <section className="border border-grey-200 bg-white">
      <div className="flex flex-wrap items-center justify-between gap-3 px-5 py-3 border-b border-grey-200">
        <div className="flex flex-wrap items-center gap-3">
        <h3 className="font-display uppercase tracking-wide text-sm">
          Encoder {encoder.encoderOrdinal} · sensor port {encoder.sensorPort}
          {encoder.sensorPort >= 3 ? ' · external' : ''}
        </h3>
        <span className="flex flex-wrap gap-1.5 text-xs text-grey-700">
          {roles.length === 0 ? (
            <span className="text-grey-500">no controller uses it</span>
          ) : (
            roles.map(role => (
              <span key={role} className="border border-grey-300 px-1.5 py-0.5">
                {role}
              </span>
            ))
          )}
        </span>
        </div>
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
      <EncoderStatusBits
        slavePosition={slavePosition}
        encoderOrdinal={encoder.encoderOrdinal}
        latched={status?.encoderStatus}
      />

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
        {restoreFilePort && (
          <label className="flex items-center gap-2 h-[38px] text-sm">
            <Checkbox checked={writeRestoreFile} onChange={setWriteRestoreFile} />
            Write the restore file
          </label>
        )}
        <button
          type="button"
          className={`${btnPrimary} h-[38px]`}
          disabled={busy || !status?.expectedOutputRegister}
          onClick={() =>
            onConfigure(
              multiturn && batteryMode !== 'default' ? batteryMode : null,
              restoreFilePort && writeRestoreFile,
            )
          }
        >
          Configure
        </button>
        <p className="text-xs text-grey-600 max-w-xl">
          Writes the EEPROM of the encoder. The calibration registers keep their values. The drive
          must not be in Operation Enabled, and the encoder reports no position while it is
          configured. Read the status first. The restore file is what the drive writes back into
          the encoder when the iC-MU reports a corrupted EEPROM. Leave it off to keep the file that
          is there now.
        </p>
      </div>
      {calibrationAvailable && (
        <IcMuCalibrationSection slavePosition={slavePosition} encoder={encoder} busy={busy} />
      )}
      <IcMuEepromSection slavePosition={slavePosition} encoder={encoder} busy={busy} />
    </section>
  )
}

function statusHex(value: number): string {
  return `0x${value.toString(16).toUpperCase().padStart(6, '0')}`
}

// The encoder status word in an input. Read fills it from the drive; a person can also type a value,
// for example one from a support log. The server decodes whatever the input holds.
function EncoderStatusBits({
  slavePosition,
  encoderOrdinal,
  latched,
}: {
  slavePosition: number
  encoderOrdinal: number
  /** What the last status run read, if it ran. */
  latched: number | undefined
}) {
  const { api } = useConnection()
  const index = encoderOrdinal === 1 ? 0x2111 : 0x2113
  const object = `0x${index.toString(16)}:04`
  const [text, setText] = useState('')
  const [typed, setTyped] = useState(false)

  useEffect(() => {
    if (latched !== undefined) {
      setText(statusHex(latched))
      setTyped(false)
    }
  }, [latched])

  const read = useMutation({
    mutationFn: () => api.readParameter(slavePosition, index, 4).then(r => r.data),
    onSuccess: parameter => {
      setText(statusHex(Number(parameter.value)))
      setTyped(false)
    },
  })

  const value = text.trim()
  const decoded = useQuery({
    queryKey: ['ic-mu-encoder-status', value],
    queryFn: () => api.getIcMuEncoderStatus({ encoderStatus: value }).then(r => r.data),
    enabled: value !== '',
    retry: false,
  })

  return (
    <div className="px-5 py-4 border-t border-grey-100 text-sm">
      <div className="flex flex-wrap items-end gap-3">
        <div>
          <label className={labelCls} htmlFor={`encoder-status-${encoderOrdinal}`}>
            Encoder status ({object})
          </label>
          <input
            id={`encoder-status-${encoderOrdinal}`}
            className={`${inputCls} font-mono w-40`}
            placeholder="0x000000"
            value={text}
            onChange={event => {
              setText(event.target.value)
              setTyped(true)
            }}
          />
        </div>
        <button
          type="button"
          className={`${btnOutline} h-[38px]`}
          disabled={read.isPending}
          onClick={() => read.mutate()}
        >
          Read
        </button>
        {value !== '' && (
          <span className={`text-xs ${typed ? 'text-status-warn' : 'text-grey-600'}`}>
            {typed ? 'Typed, not read from the drive' : 'Read from the drive'}
          </span>
        )}
      </div>
      <p className="text-xs text-grey-600 mt-2 mb-2 max-w-3xl">
        What the drive latched at the last BiSS error or at start-up, read with an SDO upload, not a
        live view of the chips. It keeps its value until the drive restarts, or until a change of
        the encoder configuration object restarts the encoder. The iC-MU encoder configuration does
        not clear it. Type a value to decode one from a log.
      </p>
      {read.isError && <p className="text-xs text-status-bad">{apiErrorMessage(read.error)}</p>}
      {decoded.isError && (
        <p className="text-xs text-status-bad">{apiErrorMessage(decoded.error)}</p>
      )}
      {decoded.data &&
        (decoded.data.encoderStatusBits.length === 0 ? (
          <p className="text-grey-600">No error latched.</p>
        ) : (
          <ul className="space-y-2">
            {decoded.data.encoderStatusBits.map(bit => (
              <li key={bit.bit} className="border-l-2 border-status-bad pl-3">
                <div className="flex flex-wrap items-baseline gap-x-2">
                  <span className="font-mono font-semibold">{bit.name ?? `bit ${bit.bit}`}</span>
                  <span className="text-xs text-grey-600">
                    {bit.chip ? `${bit.chip} ${bit.register} bit ${bit.registerBit}` : `bit ${bit.bit}`}
                  </span>
                  {bit.title && <span className="text-grey-800">{bit.title}</span>}
                </div>
                {bit.name === null ? (
                  <p className="text-xs text-grey-600">
                    The drive never latches this bit, so this is not a value the drive wrote.
                  </p>
                ) : (
                  <>
                    {bit.description && <p className="text-xs text-grey-700">{bit.description}</p>}
                    {bit.remedy && (
                      <p className="text-xs text-grey-900">
                        <span className="font-semibold">What to do: </span>
                        {bit.remedy}
                      </p>
                    )}
                  </>
                )}
              </li>
            ))}
          </ul>
        ))}
    </div>
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
