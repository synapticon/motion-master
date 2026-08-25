import { useState } from 'react'
import { useParams } from 'react-router'
import { useQuery } from '@tanstack/react-query'
import type { FsoeConnection } from '@synapticon/motion-master-client'
import DevicePageHeader from '../components/DevicePageHeader'
import Callout from '../components/Callout'
import FsoeConnectionPanel from '../components/FsoeConnectionPanel'
import SafetyControlPanel from '../components/SafetyControlPanel'
import SafeProcessValuesPanel from '../components/SafeProcessValuesPanel'
import SafeSensorPanel from '../components/SafeSensorPanel'
import Ss1Panel from '../components/Ss1Panel'
import Ss1StopTracePanel from '../components/Ss1StopTracePanel'
import { useConnection } from '../contexts/ConnectionContext'

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

/**
 * A rule above a run of related panels.
 *
 * The page carries six sections and no single one of them is the subject; without a coarser
 * grouping the reader has to hold all six titles at once to find anything. Four named groups -
 * link, control, measurements, functions - is the structure somebody already has in their head
 * when they arrive.
 */
function Group({ label }: { label: string }) {
  return (
    <div className="flex items-center gap-3 pt-2">
      <span className="text-[10px] uppercase tracking-[0.2em] text-grey-400">{label}</span>
      <span className="flex-1 h-px bg-grey-200" aria-hidden />
    </div>
  )
}

export default function DeviceSafetyPage() {
  const { deviceId } = useParams()
  const slavePosition = Number(deviceId)
  const { api } = useConnection()

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

  const report = (text: string) => {
    setMessage(text)
    setError(null)
  }
  const fail = (err: unknown) => {
    setError(apiError(err))
    setMessage(null)
  }

  return (
    <>
      <DevicePageHeader
        slavePosition={slavePosition}
        title="Safety over EtherCAT"
        description={
          <>
            Opens an FSoE connection to this drive, drives its safety functions, and shows the safe
            process values it publishes. The Safety PDU travels in the cyclic process data, so the
            bus has to be mapped and in OP first.
          </>
        }
      />

      <div className="px-8 py-6 space-y-4">
        <Callout variant="danger">
          <strong>This is a protocol master, not a safety master.</strong> It implements ETG.5100;
          it does not implement the integrity of the machine running it, and it cannot - that needs
          certified hardware. What makes it useful anyway is that the drive stays safe on its own:
          it authenticates every frame and drops its outputs to the safe state when the frames stop,
          whatever this master does. Use this page to commission, to diagnose, and to move a safe
          axis on a bench. Do not use it as the safety function of a machine.
        </Callout>

        {error && <Callout variant="error">{error}</Callout>}
        {message && !error && <Callout variant="info">{message}</Callout>}

        <Group label="Link" />
        <FsoeConnectionPanel
          slavePosition={slavePosition}
          connection={connection}
          onMessage={report}
          onError={fail}
        />

        {connection && (
          <>
            <Group label="Control" />
            <SafetyControlPanel
              slavePosition={slavePosition}
              connection={connection}
              onMessage={report}
              onError={fail}
            />
          </>
        )}

        <Group label="Measurements" />
        {connection && <SafeProcessValuesPanel connection={connection} />}
        {/* Outside the connection block on purpose. These objects read over SDO,
            and the moment they are most wanted is when the connection will NOT
            open - a wrong safety address, a tolerance nobody set - which is
            exactly when there is no connection to hang them off. */}
        <SafeSensorPanel slavePosition={slavePosition} />

        <Group label="Safety functions" />
        <Ss1Panel slavePosition={slavePosition} />
        {/* Directly under the SS1 parameters it is a picture of. It needs a connection to have
            anything to show and says so when there is none, which is a better outcome than being
            rendered somewhere it makes no sense. */}
        <Ss1StopTracePanel slavePosition={slavePosition} />
      </div>
    </>
  )
}
