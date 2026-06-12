import { useState } from 'react'
import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query'
import type { DeviceDiagnostics, PortDiagnostics } from '@mm/api-client'
import PageHeader from '../components/PageHeader'
import DiagnosticsExplainer from '../components/DiagnosticsExplainer'
import SlavePositionBadge from '../components/SlavePositionBadge'
import { useConnection } from '../contexts/ConnectionContext'
import { usePreferences } from '../contexts/PreferencesContext'
import { btnOutline } from '../utils/styles'

// Unwraps the {error: {error: "..."}} shape the generated client rejects with.
function apiError(err: unknown): string {
  if (err && typeof err === 'object' && 'error' in err) {
    const inner = (err as { error: unknown }).error
    if (inner && typeof inner === 'object' && 'error' in inner) {
      return String((inner as { error: unknown }).error)
    }
  }
  return 'Unknown error'
}

// Trims the float milliseconds (ticks × base, e.g. 100.0) to a tidy string.
const formatMs = (ms: number): string => Number(ms.toFixed(3)).toString()

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
  const { hintsInline } = usePreferences()
  return (
    <div title={hintsInline ? undefined : hint} className={hintsInline ? undefined : 'cursor-help'}>
      <p className="text-[10px] uppercase tracking-wide text-grey-500 font-display">{label}</p>
      <p className={`font-mono text-sm mt-0.5 ${value > 0 ? 'text-status-bad font-bold' : 'text-grey-800'}`}>
        {value}
      </p>
      {hintsInline && <p className="text-[11px] text-grey-500 mt-1 leading-snug">{hint}</p>}
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

// A read-only labelled cell for the watchdog status/config breakdown.
function Stat({ label, value, hint, cls }: { label: string; value: string; hint?: string; cls?: string }) {
  const { hintsInline } = usePreferences()
  const tooltip = hint && !hintsInline
  return (
    <div title={tooltip ? hint : undefined} className={tooltip ? 'cursor-help' : undefined}>
      <p className="text-[10px] uppercase tracking-wide text-grey-500 font-display">{label}</p>
      <p className={`font-mono text-sm mt-0.5 ${cls ?? 'text-grey-800'}`}>{value}</p>
      {hint && hintsInline && <p className="text-[11px] text-grey-500 mt-1 leading-snug">{hint}</p>}
    </div>
  )
}

// The PD watchdog is the dead-man's switch behind the "PD watchdog" expiration counter above it:
// a device in OP that misses process data past this timeout faults itself to SAFE-OP+error, and
// raising it lets a device tolerate the brief whole-bus PDO pause of a re-map. This panel shows how
// the timeout is configured (ticks × the divider's time base) and its live run state, and lets the
// user change it. Per-device (config lives in the slave's ESC), so its own query/mutation rather
// than the page's bulk diagnostics fetch; polls on the same 2 s cadence so the run state stays
// live. A 0 ms timeout disables the watchdog.
function WatchdogControl({ slavePosition, expirations }: { slavePosition: number; expirations: number }) {
  const { api } = useConnection()
  const queryClient = useQueryClient()
  // null = follow the fetched value; a string = the user is editing.
  const [draft, setDraft] = useState<string | null>(null)

  const query = useQuery({
    queryKey: ['watchdog', slavePosition],
    queryFn: () => api.getProcessDataWatchdog(slavePosition).then(r => r.data),
    refetchInterval: 2000,
  })

  const mutation = useMutation({
    mutationFn: (timeoutMs: number) =>
      api.setProcessDataWatchdog(slavePosition, { timeoutMs }).then(r => r.data),
    onSuccess: data => {
      queryClient.setQueryData(['watchdog', slavePosition], data)
      setDraft(null)
    },
  })

  const wd = query.data
  const shown = draft ?? (wd ? formatMs(wd.timeoutMs) : '')
  const value = Number(shown)
  const canSet = shown !== '' && Number.isFinite(value) && value >= 0 && !mutation.isPending

  // Live run state: disabled (ticks 0), running (counting), or expired (timed out / not yet fed).
  const state = !wd ? '' : !wd.enabled ? 'Disabled' : wd.running ? 'Running' : 'Expired'
  const stateCls = !wd
    ? ''
    : !wd.enabled
      ? 'text-grey-500'
      : wd.running
        ? 'text-status-good'
        : 'text-status-bad font-bold'
  // The divider sets the tick length: 40 ns × (divider + 2). Shown so the timeout derivation
  // (ticks × this base) is transparent.
  const tickBaseUs = wd ? Number(((40 * (wd.divider + 2)) / 1000).toFixed(3)) : 0

  return (
    <div className="space-y-2">
      <p className="eyebrow">Process-data watchdog</p>
      {query.isError ? (
        <p className="text-xs text-grey-500">Not available on this transport.</p>
      ) : query.isPending || !wd ? (
        <p className="text-xs text-grey-600">Loading…</p>
      ) : (
        <>
          <div className="grid grid-cols-2 sm:grid-cols-4 gap-4 border border-grey-200 p-3 bg-grey-50">
            <Stat
              label="State"
              value={state}
              cls={stateCls}
              hint="Status register 0x0440 bit 0: running = counting, expired = timed out (or not yet fed), disabled = timeout 0"
            />
            <Stat
              label="Timeout"
              value={wd.enabled ? `${formatMs(wd.timeoutMs)} ms` : 'disabled'}
              hint="Process-data watchdog timeout (0x0420 × the tick base). On expiry a device in OP drops to SAFE-OP+error."
            />
            <Stat
              label="Derivation"
              value={`${wd.ticks} × ${tickBaseUs} µs`}
              hint={`Timeout = ticks × tick base. Ticks = ${wd.ticks} (register 0x0420). Tick base = ${tickBaseUs} µs = (divider + 2) × 40 ns, where 40 ns is one period of the ESC's 25 MHz reference clock and the divider (${wd.divider}, register 0x0400) sets how many of those periods make one watchdog tick; the +2 is a fixed hardware offset in the ESC.`}
            />
            <Stat
              label="Expirations"
              value={String(expirations)}
              cls={expirations > 0 ? 'text-status-bad font-bold' : 'text-grey-800'}
              hint="Cumulative process-data watchdog expirations (0x0442) since the last power cycle"
            />
          </div>

          <div className="flex flex-wrap items-end gap-3">
            <div>
              <label className="text-[10px] uppercase tracking-wide text-grey-500 font-display block mb-1">
                Set timeout (ms) · 0 disables
              </label>
              <input
                type="number"
                min={0}
                step="any"
                value={shown}
                onChange={e => setDraft(e.target.value)}
                className="border border-grey-300 px-3 py-1.5 text-sm w-32 bg-white font-mono"
              />
            </div>
            <button onClick={() => mutation.mutate(value)} disabled={!canSet} className={btnOutline}>
              {mutation.isPending ? 'Setting…' : 'Set'}
            </button>
          </div>

          {mutation.isError && (
            <p className="text-xs text-status-bad font-mono">{apiError(mutation.error)}</p>
          )}
          {mutation.isSuccess && draft === null && (
            <p className="text-xs text-status-good font-mono">
              Programmed {wd.enabled ? `${formatMs(wd.timeoutMs)} ms` : 'disabled'} (rounded to the
              tick base).
            </p>
          )}
        </>
      )}
    </div>
  )
}

function DeviceCard({ device }: { device: DeviceDiagnostics }) {
  const errors = deviceErrors(device)
  return (
    <section className="border border-grey-200">
      <header className="border-b border-grey-200 bg-grey-50 px-4 py-3 flex flex-wrap items-baseline gap-x-3 gap-y-1">
        <SlavePositionBadge position={device.slavePosition} />
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
        {/* PD watchdog expirations live in the watchdog panel below, alongside its config/state. */}
        <div className="grid grid-cols-2 sm:grid-cols-3 gap-4">
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
            label="PDI watchdog"
            value={device.pdiWatchdog}
            hint="PDI watchdog expirations (0x0443)"
          />
        </div>

        <WatchdogControl
          slavePosition={device.slavePosition}
          expirations={device.processDataWatchdog}
        />

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
        <DiagnosticsExplainer />

        <div className="flex justify-end">
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
