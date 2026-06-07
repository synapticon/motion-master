import { useQuery } from '@tanstack/react-query'
import type { DcSyncStatus } from '@mm/api-client'
import PageHeader from '../components/PageHeader'
import SlavePositionBadge from '../components/SlavePositionBadge'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline } from '../utils/styles'

// Once the drift-compensation loop settles, a locked slave tracks the reference clock to within a
// few tens of nanoseconds; we treat anything under 1 µs as synced and flag a larger deviation. The
// reference clock and non-DC slaves are not measured against this.
const SYNC_TOLERANCE_NS = 1000

const inSync = (d: DcSyncStatus): boolean =>
  !d.dcCapable || d.referenceClock || Math.abs(d.systemTimeDifference) < SYNC_TOLERANCE_NS

// Format a nanosecond figure compactly: bare ns up to a microsecond, then µs / ms with the sign
// preserved so the reader can see which way a slave's clock leans relative to the reference.
function formatNs(ns: number): string {
  const sign = ns < 0 ? '−' : ''
  const abs = Math.abs(ns)
  if (abs < 1_000) return `${sign}${abs} ns`
  if (abs < 1_000_000) return `${sign}${(abs / 1_000).toFixed(2)} µs`
  return `${sign}${(abs / 1_000_000).toFixed(2)} ms`
}

function Difference({ device }: { device: DcSyncStatus }) {
  if (!device.dcCapable) {
    return <span className="text-grey-400">—</span>
  }
  if (device.referenceClock) {
    return <span className="text-grey-500 font-mono">0 ns (reference)</span>
  }
  const synced = inSync(device)
  return (
    <span className={`font-mono ${synced ? 'text-grey-800' : 'text-status-bad font-bold'}`}>
      {formatNs(device.systemTimeDifference)}
    </span>
  )
}

function RoleBadge({ device }: { device: DcSyncStatus }) {
  if (!device.dcCapable) {
    return <span className="text-[11px] font-display uppercase tracking-wide text-grey-400">no DC</span>
  }
  if (device.referenceClock) {
    return (
      <span className="text-[11px] font-display uppercase tracking-wide text-syn-red">reference</span>
    )
  }
  return (
    <span className="text-[11px] font-display uppercase tracking-wide text-grey-500">follower</span>
  )
}

export default function DcSyncPage() {
  const { api } = useConnection()

  const query = useQuery({
    queryKey: ['dcSync'],
    queryFn: () => api.getDcSync(),
    refetchInterval: 2000,
  })

  const devices = query.data?.data ?? []
  const dcDevices = devices.filter(d => d.dcCapable)
  const unsynced = dcDevices.filter(d => !inSync(d))
  const reference = dcDevices.find(d => d.referenceClock)

  const th = 'px-4 py-2 font-display uppercase tracking-wide font-medium'

  return (
    <div>
      <PageHeader
        eyebrow="Fieldbus"
        title="DC Sync"
        description={
          <>
            Live distributed-clock synchronisation read from each slave's EtherCAT Slave Controller
            (system-time delay 0x0928, system-time difference 0x092C). The reference clock — the
            first DC-capable slave — defines bus time; every other DC slave continuously corrects
            its local clock toward it. The difference converges toward zero once the bus has been
            exchanging in SAFE-OP/OP long enough for the drift loops to settle — watch for a slave
            whose deviation stays large or grows rather than the absolute value. Refreshes every 2
            seconds.
          </>
        }
      />
      <div className="p-4 sm:p-8 space-y-6">
        <div className="flex items-center justify-between gap-4">
          {dcDevices.length > 0 ? (
            <p className="text-xs text-grey-600">
              {dcDevices.length} DC-capable {dcDevices.length === 1 ? 'slave' : 'slaves'}
              {reference ? `, reference clock at #${reference.slavePosition}` : ', no reference clock'}.{' '}
              {unsynced.length === 0 ? (
                <span className="text-status-good">All locked to the reference.</span>
              ) : (
                <span className="text-status-bad font-medium">
                  {unsynced.length} not locked (&gt; {SYNC_TOLERANCE_NS} ns off).
                </span>
              )}
            </p>
          ) : (
            <span />
          )}
          <button
            onClick={() => query.refetch()}
            disabled={query.isFetching}
            className={btnOutline}
          >
            {query.isFetching ? 'Loading…' : 'Refresh'}
          </button>
        </div>

        {query.isError && (
          <p className="text-xs text-status-bad font-mono">
            Failed to load DC sync status. The transport may have no EtherCAT Slave Controller (e.g.
            SPoE), or no devices have been scanned.
          </p>
        )}

        {query.isFetching && !query.data && <p className="text-xs text-grey-600">Loading…</p>}

        {query.data && devices.length === 0 && (
          <p className="text-xs text-grey-600">
            No devices. Scan the bus to enumerate devices, then return here.
          </p>
        )}

        {devices.length > 0 && (
          <div className="border border-grey-200 overflow-x-auto">
            <table className="w-full min-w-[560px] text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50 text-left text-grey-600">
                  <th className={th}>Slave</th>
                  <th className={th}>Device</th>
                  <th className={th}>Role</th>
                  <th
                    className={`${th} cursor-help`}
                    title="System-time delay / propagation delay (0x0928): time for the reference clock's signal to reach this slave"
                  >
                    Propagation
                  </th>
                  <th
                    className={`${th} cursor-help`}
                    title="System-time difference (0x092C): signed deviation of this slave's local clock from the reference. Positive = ahead, negative = behind"
                  >
                    Difference
                  </th>
                </tr>
              </thead>
              <tbody>
                {devices.map(device => (
                  <tr key={device.slavePosition} className="border-b border-grey-100 last:border-0">
                    <td className="px-4 py-2">
                      <SlavePositionBadge position={device.slavePosition} />
                    </td>
                    <td className="px-4 py-2 text-grey-800">
                      {device.deviceName || <span className="text-grey-400">unknown device</span>}
                    </td>
                    <td className="px-4 py-2">
                      <RoleBadge device={device} />
                    </td>
                    <td className="px-4 py-2">
                      {device.dcCapable ? (
                        <span className="font-mono text-grey-600">
                          {formatNs(device.propagationDelay)}
                        </span>
                      ) : (
                        <span className="text-grey-400">—</span>
                      )}
                    </td>
                    <td className="px-4 py-2">
                      <Difference device={device} />
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </div>
    </div>
  )
}
