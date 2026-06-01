import { useQuery } from '@tanstack/react-query'
import type { DeviceDiagnostics, PortDiagnostics } from '@mm/api-client'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'

const btnOutlineCls =
  'border border-syn-red text-syn-red px-3 py-1.5 text-xs hover:bg-syn-red hover:text-white disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

// A port is worth showing when it carries a link or has accumulated any error — unused ports on a
// 4-port ESC stay linkless with zero counters and would only add noise.
const portInUse = (p: PortDiagnostics): boolean =>
  p.linkUp || p.invalidFrame > 0 || p.rxError > 0 || p.forwardedError > 0 || p.lostLink > 0

const portErrors = (p: PortDiagnostics): number =>
  p.invalidFrame + p.rxError + p.forwardedError + p.lostLink

const deviceErrors = (d: DeviceDiagnostics): number =>
  d.ports.reduce((sum, p) => sum + portErrors(p), 0) +
  d.processingUnitError +
  d.pdiError +
  d.processDataWatchdog +
  d.pdiWatchdog

// Non-zero counters are the signal — render them bold red, zeros muted. The counters are
// cumulative since the last clear, so a steady non-zero value is historical; the page polls so the
// reader can watch for one that climbs.
function Counter({ value, title }: { value: number; title?: string }) {
  return (
    <span
      title={title}
      className={`font-mono ${value > 0 ? 'text-status-bad font-bold' : 'text-grey-400'}`}
    >
      {value}
    </span>
  )
}

function Bool({ value, good }: { value: boolean; good: boolean }) {
  if (value === good) {
    return <span className="text-status-good">yes</span>
  }
  return <span className="text-status-bad font-medium">no</span>
}

function Field({ label, value, hint }: { label: string; value: number; hint: string }) {
  return (
    <div title={hint} className="cursor-help">
      <p className="text-[10px] uppercase tracking-wide text-grey-500 font-display">{label}</p>
      <p className={`font-mono text-sm mt-0.5 ${value > 0 ? 'text-status-bad font-bold' : 'text-grey-800'}`}>
        {value}
      </p>
    </div>
  )
}

function PortTable({ ports }: { ports: PortDiagnostics[] }) {
  // Keep the real ESC port number (index) even after filtering unused ports out.
  const rows = ports.map((p, index) => ({ p, index })).filter(({ p }) => portInUse(p))
  if (rows.length === 0) {
    return <p className="text-xs text-grey-500">No active ports.</p>
  }
  const th = 'px-4 py-2 font-display uppercase tracking-wide font-medium'
  return (
    <div className="border border-grey-200 overflow-x-auto">
      <table className="w-full min-w-[620px] text-xs border-collapse">
        <thead>
          <tr className="border-b border-grey-200 bg-grey-50 text-left text-grey-600">
            <th className={th}>Port</th>
            <th className={th}>Link</th>
            <th className={`${th} cursor-help`} title="Loop closed — no downstream slave, or port disabled">
              Loop
            </th>
            <th className={`${th} cursor-help`} title="Stable communication established on this port">
              Comm
            </th>
            <th className={`${th} cursor-help`} title="Invalid-frame counter (0x0300+): frames with a bad FCS/structure">
              Invalid
            </th>
            <th className={`${th} cursor-help`} title="Physical-layer RX error counter (0x0301+): RX_ER asserted by the PHY">
              RX err
            </th>
            <th className={`${th} cursor-help`} title="Forwarded RX error counter (0x0308+): errors first flagged upstream — points at the segment where corruption began">
              Fwd err
            </th>
            <th className={`${th} cursor-help`} title="Lost-link counter (0x0310+): link-down events on this port">
              Lost link
            </th>
          </tr>
        </thead>
        <tbody>
          {rows.map(({ p, index }) => (
            <tr key={index} className="border-b border-grey-100 last:border-0">
              <td className="px-4 py-2 font-mono">{index}</td>
              <td className="px-4 py-2">
                <Bool value={p.linkUp} good={true} />
              </td>
              <td className="px-4 py-2 font-mono text-grey-600">{p.loopClosed ? 'closed' : 'open'}</td>
              <td className="px-4 py-2">
                <Bool value={p.communication} good={true} />
              </td>
              <td className="px-4 py-2">
                <Counter value={p.invalidFrame} />
              </td>
              <td className="px-4 py-2">
                <Counter value={p.rxError} />
              </td>
              <td className="px-4 py-2">
                <Counter value={p.forwardedError} />
              </td>
              <td className="px-4 py-2">
                <Counter value={p.lostLink} />
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

function DeviceCard({ device }: { device: DeviceDiagnostics }) {
  const errors = deviceErrors(device)
  return (
    <section className="border border-grey-200">
      <header className="border-b border-grey-200 bg-grey-50 px-4 py-3 flex flex-wrap items-baseline gap-x-3 gap-y-1">
        <span className="font-mono text-sm text-grey-800">#{device.slavePosition}</span>
        <span className="text-sm text-grey-800 font-medium">
          {device.deviceName || <span className="text-grey-400">unknown device</span>}
        </span>
        <span className="ml-auto text-[11px] font-display uppercase tracking-wide">
          {errors > 0 ? (
            <span className="text-status-bad">{errors} error{errors === 1 ? '' : 's'}</span>
          ) : (
            <span className="text-status-good">healthy</span>
          )}
        </span>
      </header>

      <div className="p-4 space-y-5">
        <div className="grid grid-cols-2 sm:grid-cols-4 gap-4">
          <Field
            label="Processing-unit err"
            value={device.processingUnitError}
            hint="ECAT processing-unit error counter (0x030C): datagrams reaching the unit malformed"
          />
          <Field
            label="PDI err"
            value={device.pdiError}
            hint="PDI error counter (0x030D): problems on the slave-local process-data interface"
          />
          <Field
            label="PD watchdog"
            value={device.processDataWatchdog}
            hint="Process-data (SM) watchdog expirations (0x0442): the slave stopped seeing fresh outputs"
          />
          <Field
            label="PDI watchdog"
            value={device.pdiWatchdog}
            hint="PDI watchdog expirations (0x0443)"
          />
        </div>

        <div className="space-y-2">
          <p className="eyebrow">Ports</p>
          <PortTable ports={device.ports} />
        </div>
      </div>
    </section>
  )
}

export default function BusDiagnosticsPage() {
  const { api } = useConnection()

  const query = useQuery({
    queryKey: ['deviceDiagnostics'],
    queryFn: () => api.getDeviceDiagnostics(),
    refetchInterval: 2000,
  })

  const devices = query.data?.data ?? []

  return (
    <div>
      <PageHeader
        eyebrow="Fieldbus"
        title="Diagnostics"
        description={
          <>
            Live link-quality and watchdog counters read from each slave's EtherCAT Slave Controller
            (DL Status, error-counter and watchdog register blocks). The counters are cumulative
            since the last power cycle — a steady value is historical; watch for one that climbs.
            Per-port counters localise a degrading link to a specific cable, and the watchdog
            counters distinguish a slave that stopped receiving process data from a master-side
            problem. Refreshes every 2 seconds.
          </>
        }
      />
      <div className="p-4 sm:p-8 space-y-6">
        <div className="flex justify-end">
          <button
            onClick={() => query.refetch()}
            disabled={query.isFetching}
            className={btnOutlineCls}
          >
            {query.isFetching ? 'Loading…' : 'Refresh'}
          </button>
        </div>

        {query.isError && (
          <p className="text-xs text-status-bad font-mono">
            Failed to load diagnostics. The transport may have no EtherCAT Slave Controller (e.g.
            SPoE), or no devices have been scanned.
          </p>
        )}

        {query.isFetching && !query.data && <p className="text-xs text-grey-600">Loading…</p>}

        {query.data && devices.length === 0 && (
          <p className="text-xs text-grey-600">
            No devices. Scan the bus to enumerate devices, then return here.
          </p>
        )}

        {devices.map(device => (
          <DeviceCard key={device.slavePosition} device={device} />
        ))}
      </div>
    </div>
  )
}
