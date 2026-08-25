import { useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import type { FsoeConnection } from '@synapticon/motion-master-client'
import Section from './Section'
import { btnCls, btnGhostCls, inputCls } from './controlStyles'
import { useConnection } from '../contexts/ConnectionContext'

/**
 * The FSoE link itself: opening it, keeping it, and what it reports about
 * itself.
 *
 * Everything here is about the CONNECTION and nothing here is about a safety
 * function. That split is the point of the component - the data command, the
 * reset and the close used to sit inside the Safe Torque Off block, where they
 * read as things that did something to STO. They do not; they do something to
 * the link that carries it.
 */

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

export default function FsoeConnectionPanel({
  slavePosition,
  connection,
  onMessage,
  onError,
}: {
  slavePosition: number
  connection: FsoeConnection | undefined
  onMessage: (text: string) => void
  onError: (err: unknown) => void
}) {
  const { api } = useConnection()
  const queryClient = useQueryClient()
  const invalidate = () => queryClient.invalidateQueries({ queryKey: ['fsoe'] })

  const [slaveAddress, setSlaveAddress] = useState('1')
  const [connectionId, setConnectionId] = useState('1')
  const [watchdogMs, setWatchdogMs] = useState('100')

  const openConnection = useMutation({
    mutationFn: () =>
      api.openFsoeConnection({
        slavePosition,
        slaveAddress: Number(slaveAddress),
        connectionId: Number(connectionId),
        watchdogMs: Number(watchdogMs),
      }),
    onSuccess: () => {
      onMessage('Connection opened. The handshake runs over the next few bus cycles.')
      invalidate()
    },
    onError,
  })

  const closeConnection = useMutation({
    mutationFn: () => api.closeFsoeConnection(slavePosition),
    onSuccess: () => {
      onMessage('Connection closed. The drive’s watchdog takes its outputs to the safe state.')
      invalidate()
    },
    onError,
  })

  const resetConnection = useMutation({
    mutationFn: () => api.resetFsoeConnection(slavePosition),
    onSuccess: () => {
      onMessage('Reset requested. The handshake starts again.')
      invalidate()
    },
    onError,
  })

  const setDataCommand = useMutation({
    mutationFn: (command: 'ProcessData' | 'FailSafeData') =>
      api.setFsoeDataCommand(slavePosition, { command }),
    onSuccess: (_d, command) => {
      onMessage(`Sending ${command}.`)
      invalidate()
    },
    onError,
  })

  const busy =
    openConnection.isPending ||
    closeConnection.isPending ||
    resetConnection.isPending ||
    setDataCommand.isPending

  if (!connection) {
    return (
      <Section
        title="Connection"
        chips={<span className="text-xs text-grey-500">not open</span>}
        description={
          <>
            The FSoE Slave Address has to match the address the drive itself is configured with, or
            the drive refuses the connection with <code>InvalidAddress</code>. The connection ID must
            be non-zero and unique on the bus. The watchdog bounds the whole round trip, so it has to
            exceed the bus cycle time with margin.
          </>
        }
      >
        <div className="p-4 grid gap-4 sm:grid-cols-4 items-end">
          <label className="block">
            <span className="text-[10px] uppercase tracking-wider text-grey-500">Slave address</span>
            <input
              className={inputCls}
              value={slaveAddress}
              onChange={e => setSlaveAddress(e.target.value)}
              inputMode="numeric"
            />
          </label>
          <label className="block">
            <span className="text-[10px] uppercase tracking-wider text-grey-500">Connection ID</span>
            <input
              className={inputCls}
              value={connectionId}
              onChange={e => setConnectionId(e.target.value)}
              inputMode="numeric"
            />
          </label>
          <label className="block">
            <span className="text-[10px] uppercase tracking-wider text-grey-500">Watchdog (ms)</span>
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
      </Section>
    )
  }

  return (
    <Section
      title="Connection"
      chips={
        <>
          <span
            className={`inline-flex items-center h-[18px] px-1.5 text-[10px] tracking-wide ${
              STATE_CLASS[connection.state] ?? 'bg-grey-200 text-grey-700'
            }`}
          >
            {connection.state}
          </span>
          <span
            title="A connection starts in FailSafeData and returns to it after every fault, so it has to be set to ProcessData before SafeOutputs mean anything."
            className={`inline-flex items-center h-[18px] px-1.5 text-[10px] tracking-wide cursor-help border ${
              connection.dataCommand === 'ProcessData'
                ? 'bg-status-good/10 text-status-good border-status-good/30'
                : 'bg-status-warn/10 text-status-warn border-status-warn/40'
            }`}
          >
            {connection.dataCommand}
          </span>
          {!connection.bound && (
            <span className="text-xs text-status-warn">
              unbound — the bus was re-mapped; open the connection again
            </span>
          )}
        </>
      }
      description={
        <>
          <strong>ProcessData</strong> and <strong>FailSafeData</strong> are the same frame — same
          CRC, sequence number and watchdog. Only the Command octet differs, and it says whether the
          SafeData travelling with it counts: ProcessData means use these SafeOutputs, FailSafeData
          means ignore them and hold the safe state. Both exist so a master can impose the safe
          state <em>without</em> dropping the link, which would otherwise cost a full handshake to
          recover. A connection starts in FailSafeData and returns to it after every fault, so it
          has to be set to ProcessData before any SafeOutputs mean anything.
        </>
      }
      actions={
        <>
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
          <button
            type="button"
            className={btnGhostCls}
            disabled={busy}
            onClick={() => resetConnection.mutate()}
          >
            Reset
          </button>
          <button
            type="button"
            className={btnGhostCls}
            disabled={busy}
            onClick={() => closeConnection.mutate()}
          >
            Close
          </button>
        </>
      }
    >
      <dl className="p-4 grid gap-4 grid-cols-2 sm:grid-cols-4">
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
        <Field
          label="Frame PDOs"
          value={`${hex(connection.rxPdoIndex, 4)} / ${hex(connection.txPdoIndex, 4)}`}
          title="The PDOs carrying the master frame and the slave frame."
        />
        <Field label="Cycles" value={connection.cycles.toLocaleString()} />
        <Field label="Frames accepted" value={connection.framesAccepted.toLocaleString()} />
        <Field label="Faults" value={connection.faults.toLocaleString()} />
      </dl>
    </Section>
  )
}
