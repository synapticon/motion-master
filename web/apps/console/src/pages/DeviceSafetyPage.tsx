import { useState } from 'react'
import { useParams } from 'react-router'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { FsoeConnection } from '@synapticon/motion-master-client'
import DevicePageHeader from '../components/DevicePageHeader'
import Callout from '../components/Callout'
import SafeSensorPanel from '../components/SafeSensorPanel'
import Ss1Panel from '../components/Ss1Panel'
import { useConnection } from '../contexts/ConnectionContext'

// One shared control height for every input and button, as on the Motion page: this theme's
// spacing scale is geometric, so a numeric height utility would not be ~38px.
const inputCls = 'border border-grey-300 px-3 h-[38px] text-sm w-full bg-white font-mono'
const btnCls =
  'inline-flex shrink-0 items-center justify-center whitespace-nowrap bg-syn-red text-white px-4 ' +
  'h-[38px] text-xs hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed ' +
  'cursor-pointer transition-colors'
const btnGhostCls =
  'inline-flex shrink-0 items-center justify-center whitespace-nowrap border border-grey-300 ' +
  'text-grey-700 px-4 h-[38px] text-xs hover:bg-grey-50 disabled:opacity-50 ' +
  'disabled:cursor-not-allowed cursor-pointer transition-colors'

// Fast enough to watch a handshake walk its five states, slow enough to leave the HTTP thread
// alone. The connection itself runs at the bus cycle; this is only the view of it.
const POLL_INTERVAL_MS = 300

// Flatten the client's nested {error:{error}} / {status} shape into one message.
function apiError(err: unknown): string {
  if (err && typeof err === 'object') {
    if ('error' in err) {
      const inner = (err as { error: unknown }).error
      if (inner && typeof inner === 'object' && 'error' in inner) {
        return String((inner as { error: unknown }).error)
      }
      if (typeof inner === 'string') return inner
    }
    if ('status' in err && typeof (err as { status: unknown }).status === 'number') {
      return `HTTP ${(err as { status: number }).status}`
    }
  }
  return 'Unknown error'
}

// The five connection states, coloured by what they mean for the machine rather than by progress:
// only Data carries safe data, and everything else means the drive is holding its safe state.
const STATE_CLASS: Record<string, string> = {
  Reset: 'bg-status-bad text-white',
  Session: 'bg-status-warn text-grey-900',
  Connection: 'bg-status-warn text-grey-900',
  Parameter: 'bg-status-warn text-grey-900',
  Data: 'bg-status-good text-white',
}

function hex(n: number, digits = 2): string {
  return `0x${n.toString(16).toUpperCase().padStart(digits, '0')}`
}

function octets(bytes: number[] | undefined): string {
  return (bytes ?? []).map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ')
}

function Badge({ ok, label, title }: { ok: boolean; label: string; title?: string }) {
  return (
    <span
      title={title}
      className={`inline-flex items-center h-[18px] px-1.5 text-[10px] tracking-wide ${
        ok ? 'bg-status-good text-white' : 'bg-grey-200 text-grey-600'
      } ${title ? 'cursor-help' : ''}`}
    >
      {label}
    </span>
  )
}

function Field({ label, value, title }: { label: string; value: React.ReactNode; title?: string }) {
  return (
    <div>
      <dt className="text-[10px] uppercase tracking-wider text-grey-500">{label}</dt>
      <dd title={title} className={`text-sm ${title ? 'cursor-help' : ''}`}>
        {value}
      </dd>
    </div>
  )
}

// A safe value with its own validity. The number is greyed when the drive says it is not usable,
// because a drive encodes an invalid channel as zero rather than holding its last good reading —
// so a plain "0" beside a cleared flag would read as a real measurement.
function SafeValue({
  label,
  value,
  unit,
  valid,
  extra,
}: {
  label: string
  value: string
  unit: string
  valid: boolean
  extra?: React.ReactNode
}) {
  return (
    <div className="border border-grey-200 px-4 py-3">
      <div className="flex items-center justify-between gap-2">
        <span className="text-[10px] uppercase tracking-wider text-grey-500">{label}</span>
        <Badge ok={valid} label={valid ? 'valid' : 'invalid'} />
      </div>
      <div className={`mt-1 font-mono text-xl ${valid ? 'text-grey-900' : 'text-grey-400'}`}>
        {value}
        <span className="ml-1 text-xs text-grey-500">{unit}</span>
      </div>
      {extra}
    </div>
  )
}

export default function DeviceSafetyPage() {
  const { deviceId } = useParams()
  const slavePosition = Number(deviceId)
  const { api } = useConnection()
  const queryClient = useQueryClient()

  const [slaveAddress, setSlaveAddress] = useState('1')
  const [connectionId, setConnectionId] = useState('1')
  const [watchdogMs, setWatchdogMs] = useState('100')
  const [message, setMessage] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)

  const fsoeQuery = useQuery({
    queryKey: ['fsoe'],
    queryFn: () => api.getFsoe(),
    refetchInterval: POLL_INTERVAL_MS,
  })

  const connection: FsoeConnection | undefined = fsoeQuery.data?.data.connections?.find(
    c => c.slavePosition === slavePosition,
  )

  const invalidate = () => queryClient.invalidateQueries({ queryKey: ['fsoe'] })
  const report = (text: string) => {
    setMessage(text)
    setError(null)
  }
  const fail = (err: unknown) => {
    setError(apiError(err))
    setMessage(null)
  }

  const openConnection = useMutation({
    mutationFn: () =>
      api.openFsoeConnection({
        slavePosition,
        slaveAddress: Number(slaveAddress),
        connectionId: Number(connectionId),
        watchdogMs: Number(watchdogMs),
      }),
    onSuccess: () => {
      report('Connection opened. The handshake runs over the next few bus cycles.')
      invalidate()
    },
    onError: fail,
  })

  const closeConnection = useMutation({
    mutationFn: () => api.closeFsoeConnection(slavePosition),
    onSuccess: () => {
      report('Connection closed. The drive’s watchdog takes its outputs to the safe state.')
      invalidate()
    },
    onError: fail,
  })

  const resetConnection = useMutation({
    mutationFn: () => api.resetFsoeConnection(slavePosition),
    onSuccess: () => {
      report('Reset requested. The handshake starts again.')
      invalidate()
    },
    onError: fail,
  })

  const setSto = useMutation({
    mutationFn: (released: boolean) => api.setFsoeSto(slavePosition, { released }),
    onSuccess: (_data, released) => {
      report(released ? 'STO released: the drive may produce torque.' : 'STO applied.')
      invalidate()
    },
    onError: fail,
  })

  const setDataCommand = useMutation({
    mutationFn: (command: 'ProcessData' | 'FailSafeData') =>
      api.setFsoeDataCommand(slavePosition, { command }),
    onSuccess: (_data, command) => {
      report(`Sending ${command}.`)
      invalidate()
    },
    onError: fail,
  })

  const busy =
    openConnection.isPending ||
    closeConnection.isPending ||
    resetConnection.isPending ||
    setSto.isPending ||
    setDataCommand.isPending

  const values = connection?.processValues
  const stoActive = connection?.safetyStatus?.stoActive ?? true
  const inData = connection?.state === 'Data'
  const stoReleased = ((connection?.safeOutputs?.[0] ?? 0) & 0x01) !== 0

  return (
    <>
      <DevicePageHeader
        slavePosition={slavePosition}
        title="Safety over EtherCAT"
        description={
          <>
            Opens an FSoE connection to this drive, releases Safe Torque Off, and shows the safe
            process values it publishes. The Safety PDU travels in the cyclic process data, so the
            bus has to be mapped and in OP first.
          </>
        }
      />

      <div className="px-8 py-6 space-y-6">
        <Callout variant="danger">
          <strong>This is a protocol master, not a safety master.</strong> It implements ETG.5100;
          it does not implement the integrity of the machine running it, and it cannot — that needs
          certified hardware. What makes it useful anyway is that the drive stays safe on its own:
          it authenticates every frame and drops its outputs to the safe state when the frames stop,
          whatever this master does. Use this page to commission, to diagnose, and to move a safe
          axis on a bench. Do not use it as the safety function of a machine.
        </Callout>

        {error && <Callout variant="error">{error}</Callout>}
        {message && !error && <Callout variant="info">{message}</Callout>}

        {!connection && (
          <section className="border border-grey-200">
            <header className="px-4 py-3 border-b border-grey-200">
              <h2 className="text-sm font-display tracking-wide">Open a connection</h2>
              <p className="mt-1 text-xs text-grey-600">
                The FSoE Slave Address has to match the address the drive itself is configured with,
                or the drive refuses the connection with <code>InvalidAddress</code>. The connection
                ID must be non-zero and unique on the bus. The watchdog bounds the whole round trip,
                so it has to exceed the bus cycle time with margin.
              </p>
            </header>
            <div className="p-4 grid gap-4 sm:grid-cols-4 items-end">
              <label className="block">
                <span className="text-[10px] uppercase tracking-wider text-grey-500">
                  Slave address
                </span>
                <input
                  className={inputCls}
                  value={slaveAddress}
                  onChange={e => setSlaveAddress(e.target.value)}
                  inputMode="numeric"
                />
              </label>
              <label className="block">
                <span className="text-[10px] uppercase tracking-wider text-grey-500">
                  Connection ID
                </span>
                <input
                  className={inputCls}
                  value={connectionId}
                  onChange={e => setConnectionId(e.target.value)}
                  inputMode="numeric"
                />
              </label>
              <label className="block">
                <span className="text-[10px] uppercase tracking-wider text-grey-500">
                  Watchdog (ms)
                </span>
                <input
                  className={inputCls}
                  value={watchdogMs}
                  onChange={e => setWatchdogMs(e.target.value)}
                  inputMode="numeric"
                />
              </label>
              <button
                type="button"
                className={btnCls}
                disabled={busy}
                onClick={() => openConnection.mutate()}
              >
                Open connection
              </button>
            </div>
          </section>
        )}

        {connection && (
          <>
            <section className="border border-grey-200">
              <header className="px-4 py-3 border-b border-grey-200 flex items-center gap-3">
                <h2 className="text-sm font-display tracking-wide">Connection</h2>
                <span
                  className={`inline-flex items-center h-[20px] px-2 text-[11px] tracking-wide ${
                    STATE_CLASS[connection.state] ?? 'bg-grey-200 text-grey-700'
                  }`}
                >
                  {connection.state}
                </span>
                {!connection.bound && (
                  <span className="text-xs text-status-warn">
                    unbound — the bus was re-mapped; open the connection again
                  </span>
                )}
              </header>
              <dl className="p-4 grid gap-4 grid-cols-2 sm:grid-cols-4">
                <Field
                  label="Data command"
                  value={connection.dataCommand}
                  title="A connection starts in FailSafeData and returns to it after every fault, so it has to be set to ProcessData before SafeOutputs mean anything."
                />
                <Field
                  label="Inputs"
                  value={connection.inputsValid ? 'process data' : 'fail-safe'}
                  title="Fail-safe means the octets read all zero, whether the drive sent fail-safe data or the connection left the Data state."
                />
                <Field label="Fault" value={connection.fault} />
                <Field
                  label="Drive fault code"
                  value={hex(connection.peerFaultCode)}
                  title="The code from the last Reset PDU the drive sent — the first thing to read when a handshake will not complete, because the drive saw the fault first."
                />
                <Field label="Session ID" value={hex(connection.sessionId, 4)} />
                <Field label="Slave address" value={connection.slaveAddress} />
                <Field label="Connection ID" value={connection.connectionId} />
                <Field label="Watchdog" value={`${connection.watchdogMs} ms`} />
                <Field label="Cycles" value={connection.cycles.toLocaleString()} />
                <Field label="Frames accepted" value={connection.framesAccepted.toLocaleString()} />
                <Field label="Faults" value={connection.faults.toLocaleString()} />
                <Field
                  label="Frame PDOs"
                  value={`${hex(connection.rxPdoIndex, 4)} / ${hex(connection.txPdoIndex, 4)}`}
                  title="The PDOs carrying the master frame and the slave frame."
                />
              </dl>
            </section>

            <section className="border border-grey-200">
              <header className="px-4 py-3 border-b border-grey-200">
                <h2 className="text-sm font-display tracking-wide">Safe Torque Off</h2>
                <p className="mt-1 text-xs text-grey-600">
                  Torque is permitted only when both halves agree: STO released, and the connection
                  sending <code>ProcessData</code>. The STO bit is inverted on the wire — zero
                  requests STO — so a lost frame is a request for Safe Torque Off.
                </p>
              </header>
              <div className="p-4 flex flex-wrap items-center gap-3">
                <span
                  className={`inline-flex items-center h-[20px] px-2 text-[11px] tracking-wide ${
                    stoActive ? 'bg-status-good text-white' : 'bg-syn-red text-white'
                  }`}
                  title="What the drive reports in its safety statusword, not what was asked for."
                >
                  {stoActive ? 'STO active — no torque' : 'STO released — torque permitted'}
                </span>
                <button
                  type="button"
                  className={btnCls}
                  disabled={busy || !inData || stoReleased}
                  onClick={() => setSto.mutate(true)}
                >
                  Release STO
                </button>
                <button
                  type="button"
                  className={btnGhostCls}
                  disabled={busy || !stoReleased}
                  onClick={() => setSto.mutate(false)}
                >
                  Apply STO
                </button>
                <span className="w-px h-[38px] bg-grey-200" aria-hidden />
                <button
                  type="button"
                  className={btnGhostCls}
                  disabled={busy || connection.dataCommand === 'ProcessData'}
                  onClick={() => setDataCommand.mutate('ProcessData')}
                >
                  Send ProcessData
                </button>
                <button
                  type="button"
                  className={btnGhostCls}
                  disabled={busy || connection.dataCommand === 'FailSafeData'}
                  onClick={() => setDataCommand.mutate('FailSafeData')}
                >
                  Send FailSafeData
                </button>
                <span className="w-px h-[38px] bg-grey-200" aria-hidden />
                <button
                  type="button"
                  className={btnGhostCls}
                  disabled={busy}
                  onClick={() => resetConnection.mutate()}
                >
                  Reset connection
                </button>
                <button
                  type="button"
                  className={btnGhostCls}
                  disabled={busy}
                  onClick={() => closeConnection.mutate()}
                >
                  Close
                </button>
              </div>
            </section>

            <section className="border border-grey-200">
              <header className="px-4 py-3 border-b border-grey-200 flex flex-wrap items-center gap-3">
                <h2 className="text-sm font-display tracking-wide">Safe process values</h2>
                <Badge
                  ok={values?.crossCheckOk ?? false}
                  label="cross-check"
                  title="Two sensor channels are configured and agree."
                />
                <Badge
                  ok={values?.positionReferenced ?? false}
                  label="referenced"
                  title="The safe position has an established absolute origin. Anything applying an absolute limit — Safe Limited Position above all — must gate on this, not on the position's validity flag."
                />
                {!connection.inputsValid && (
                  <span className="text-xs text-status-warn">
                    fail-safe: these are not live measurements
                  </span>
                )}
              </header>
              <div className="p-4 grid gap-4 sm:grid-cols-3">
                <SafeValue
                  label="Safe position"
                  value={(values?.positionRevolutions ?? 0).toFixed(4)}
                  unit="rev"
                  valid={(values?.positionValid ?? false) && connection.inputsValid}
                  extra={
                    <div className="mt-1 font-mono text-[11px] text-grey-500">
                      {hex(values?.position ?? 0, 8)} · 8.24 fixed point
                    </div>
                  }
                />
                <SafeValue
                  label="Safe velocity"
                  value={(values?.velocityMilliRpm ?? 0).toLocaleString()}
                  unit="mrpm"
                  valid={(values?.velocityValid ?? false) && connection.inputsValid}
                />
                <SafeValue
                  label="Safe torque"
                  value={(values?.torqueMillinewtonMetres ?? 0).toLocaleString()}
                  unit="mNm"
                  valid={(values?.torqueValid ?? false) && connection.inputsValid}
                />
              </div>
              <div className="px-4 pb-4 grid gap-4 sm:grid-cols-2">
                <Field
                  label="SafeInputs"
                  value={<span className="font-mono text-xs">{octets(connection.safeInputs)}</span>}
                  title="The SafeData octets received, after the CRC and sequence checks passed. Octet 0 is the safety statusword, octet 1 the validity bits."
                />
                <Field
                  label="SafeOutputs"
                  value={<span className="font-mono text-xs">{octets(connection.safeOutputs)}</span>}
                  title="The SafeData octets being sent. Octet 0 is the safety controlword."
                />
              </div>
            </section>
          </>
        )}

        {/* Outside the connection block on purpose. These objects read over SDO,
            and the moment they are most wanted is when the connection will NOT
            open - a wrong safety address, a tolerance nobody set - which is
            exactly when there is no connection to hang them off. */}
        <SafeSensorPanel slavePosition={slavePosition} />
        <Ss1Panel slavePosition={slavePosition} />
      </div>
    </>
  )
}
