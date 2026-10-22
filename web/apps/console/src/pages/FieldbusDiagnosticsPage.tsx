import { useState } from 'react'
import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query'
import type { CaptureSummary, DeviceDiagnostics, PortDiagnostics } from '@synapticon/motion-master-client'
import FilePickerButton from '../components/FilePickerButton'
import { encodeUserCachePath, userCacheUrl } from '../utils/userCache'
import PageHeader from '../components/PageHeader'
import DiagnosticsExplainer from '../components/DiagnosticsExplainer'
import SlavePositionBadge from '../components/SlavePositionBadge'
import { WireTiming, useWireTiming } from '../components/WireTiming'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline } from '../utils/styles'
import NoEscNotice from '../components/NoEscNotice'
import { useSupportsEsc } from '../hooks/useSupportsEsc'

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
  return (
    <div>
      <p className="text-[10px] uppercase tracking-wide text-grey-500 font-display">{label}</p>
      <p className={`font-mono text-sm mt-0.5 ${value > 0 ? 'text-status-bad font-bold' : 'text-grey-800'}`}>
        {value}
      </p>
      <p className="text-[11px] text-grey-500 mt-1 leading-snug">{hint}</p>
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
  return (
    <div>
      <p className="text-[10px] uppercase tracking-wide text-grey-500 font-display">{label}</p>
      <p className={`font-mono text-sm mt-0.5 ${cls ?? 'text-grey-800'}`}>{value}</p>
      {hint && <p className="text-[11px] text-grey-500 mt-1 leading-snug">{hint}</p>}
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
  const setTiming = useWireTiming()
  // null = follow the fetched value; a string = the user is editing.
  const [draft, setDraft] = useState<string | null>(null)

  const query = useQuery({
    queryKey: ['watchdog', slavePosition],
    queryFn: () => api.getProcessDataWatchdog(slavePosition).then(r => r.data),
    refetchInterval: 2000,
  })

  const mutation = useMutation({
    mutationFn: (timeoutMs: number) =>
      setTiming.measure(() => api.setProcessDataWatchdog(slavePosition, { timeoutMs })).then(r => r.data),
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
            <WireTiming label="Watchdog" timing={setTiming.timing} className="text-xs text-grey-500 font-mono whitespace-nowrap pb-2" />
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

// A capture is about the PC, not about a device: it records every EtherCAT frame on the bus
// adapter with the kernel's timestamp and says how late each one came back. It tells a slow PC
// from a bad cable, which the per-device counters below cannot. The server runs the capture
// program once, on request, and the file lands in the user cache under captures/, where the
// Storage page lists it and this card links it. A pcap from tcpdump on another PC gets the same
// summary once it is uploaded there.
const CAPTURES_FOLDER = 'captures'

function KindRow({ label, k }: { label: string; k: CaptureSummary['control'] }) {
  const band = (n: number) => (n > 0 ? 'text-status-bad font-bold' : 'text-grey-400')
  return (
    <tr className="border-t border-grey-100">
      <td className="px-3 py-1 text-grey-700">{label}</td>
      <td className="px-3 py-1 font-mono">{k.sent}</td>
      <td className="px-3 py-1 font-mono">{k.resent}</td>
      <td className={`px-3 py-1 font-mono ${band(k.lost)}`}>{k.lost}</td>
      <td className={`px-3 py-1 font-mono ${band(k.over2Ms)}`}>{k.over2Ms}</td>
      <td className={`px-3 py-1 font-mono ${band(k.over20Ms)}`}>{k.over20Ms}</td>
      <td className={`px-3 py-1 font-mono ${band(k.over100Ms)}`}>{k.over100Ms}</td>
      <td className={`px-3 py-1 font-mono ${k.latestUs > 100000 ? 'text-status-bad font-bold' : k.latestUs > 20000 ? 'text-status-warn' : ''}`}>
        {(k.latestUs / 1000).toFixed(1)} ms
      </td>
    </tr>
  )
}

function SummaryTable({ summary }: { summary: CaptureSummary }) {
  const seconds = ((summary.lastUs - summary.firstUs) / 1e6).toFixed(1)
  return (
    <div className="space-y-2">
      <p className="text-xs text-grey-600">
        {summary.frames} frames over {seconds} s
        {summary.notEtherCat > 0 && <>, {summary.notEtherCat} not EtherCAT</>}
        {summary.unmatchedReturns > 0 && <>, {summary.unmatchedReturns} returns with no send to match</>}.
      </p>
      <table className="w-full text-xs">
        <thead>
          <tr className="text-left text-grey-500">
            <th className="px-3 py-1 font-normal">Kind</th>
            <th className="px-3 py-1 font-normal">Sent</th>
            <th className="px-3 py-1 font-normal">Resent</th>
            <th className="px-3 py-1 font-normal">Never came back</th>
            <th className="px-3 py-1 font-normal">&gt; 2 ms</th>
            <th className="px-3 py-1 font-normal">&gt; 20 ms</th>
            <th className="px-3 py-1 font-normal">&gt; 100 ms</th>
            <th className="px-3 py-1 font-normal">Latest</th>
          </tr>
        </thead>
        <tbody>
          <KindRow label="Control plane (mailbox and registers)" k={summary.control} />
          <KindRow label="Cyclic (process data)" k={summary.cyclic} />
        </tbody>
      </table>
    </div>
  )
}

function CaptureSection() {
  const { api } = useConnection()
  const [seconds, setSeconds] = useState('30')
  const [result, setResult] = useState<{ path: string; summary: CaptureSummary } | null>(null)
  const [error, setError] = useState<string | null>(null)

  const run = useMutation({
    mutationFn: (n: number) => api.runCapture({ seconds: n }).then(r => r.data),
    onMutate: () => {
      setError(null)
    },
    onSuccess: data => setResult({ path: data.path, summary: data.summary }),
    onError: err => setError(apiError(err)),
  })

  // A pcap from another PC: into captures/ through the user cache, then the same summary.
  const upload = useMutation({
    mutationFn: async (file: File) => {
      const path = `${CAPTURES_FOLDER}/${file.name}`
      const res = await fetch(userCacheUrl(api.baseUrl, path), {
        method: 'PUT',
        headers: { 'Content-Type': 'application/octet-stream' },
        body: file,
      })
      if (!res.ok) {
        const body = await res.json().catch(() => null)
        throw new Error(body?.error ?? `HTTP ${res.status}`)
      }
      const summary = await api.getCaptureSummary(file.name).then(r => r.data)
      return { path: summary.path, summary: summary.summary }
    },
    onMutate: () => {
      setError(null)
    },
    onSuccess: data => setResult(data),
    onError: err => setError(err instanceof Error ? err.message : apiError(err)),
  })

  const n = Number(seconds)
  const canRun = Number.isFinite(n) && n >= 1 && n <= 120 && !run.isPending && !upload.isPending
  const busy = run.isPending || upload.isPending

  return (
    <section className="border border-grey-200 bg-white">
      <div className="px-4 py-3 border-b border-grey-200">
        <h3 className="font-display uppercase text-sm">Capture</h3>
        <p className="text-xs text-grey-600 mt-0.5">
          Record every EtherCAT frame on the bus adapter and see how late each one came back. This
          is about the PC, not about a device.
        </p>
      </div>
      <div className="p-4 space-y-4">
        <p className="text-xs text-grey-600 leading-relaxed">
          Motion Master runs the capture program once, for the seconds you choose, on the adapter
          the bus is on. The program records with the kernel&apos;s timestamps, adds no frame to
          the bus, and writes a pcap file under <span className="font-mono">captures/</span> in
          the user cache, so the Storage page lists it and Wireshark opens it. The summary matches
          each frame the master sent to the one that came back, per kind: the cyclic process-data
          frame, and the control plane, which is every mailbox and register request. The bands are
          the three waits the driver uses, and they are cumulative. The time is taken at the
          boundary to the NIC driver, so it covers the NIC, the wire and the device, and not the
          PC&apos;s own queues between that boundary and Motion Master. Late frames here put the
          delay on the wire or in the device. A clean capture beside &ldquo;got no frame
          back&rdquo; warnings in the server log puts it inside the PC. A pcap from tcpdump on
          another PC gets the same summary: pick the file.
        </p>
        <div className="flex flex-wrap items-center gap-3">
          <label className="text-xs text-grey-600">
            Seconds
            <input
              type="number"
              min={1}
              max={120}
              value={seconds}
              onChange={e => setSeconds(e.target.value)}
              className="ml-2 w-20 h-[38px] border border-grey-300 px-2 font-mono text-sm"
            />
          </label>
          <button onClick={() => run.mutate(n)} disabled={!canRun} className={btnOutline}>
            {run.isPending ? `Recording ${n} s…` : 'Start'}
          </button>
          <FilePickerButton
            onFile={file => upload.mutate(file)}
            accept=".pcap,application/vnd.tcpdump.pcap,application/octet-stream"
            disabled={busy}
            className="h-[38px]"
            title="Upload a pcap into captures/ in the user cache and summarise it"
          >
            {upload.isPending ? 'Summarising…' : 'Summarise a pcap'}
          </FilePickerButton>
        </div>
        {error && <p className="text-xs text-status-bad font-mono">{error}</p>}
        {busy && !error && (
          <p className="text-xs text-grey-600">
            {run.isPending ? 'Recording. The summary appears when the time is up.' : 'Reading the file…'}
          </p>
        )}
        {result && (
          <div className="space-y-2">
            <SummaryTable summary={result.summary} />
            <p className="text-xs text-grey-600">
              File: <span className="font-mono">{result.path}</span>{' '}
              <a
                className="underline"
                href={userCacheUrl(api.baseUrl, encodeUserCachePath(result.path))}
                download
              >
                Download
              </a>
            </p>
          </div>
        )}
      </div>
    </section>
  )
}

export default function FieldbusDiagnosticsPage() {
  const { api } = useConnection()
  const { timing, measure } = useWireTiming()

  const supportsEsc = useSupportsEsc()

  const query = useQuery({
    queryKey: ['deviceDiagnostics'],
    enabled: supportsEsc,
    queryFn: () => measure(() => api.getDeviceDiagnostics()),
    refetchInterval: 2000,
  })

  const devices = query.data?.data ?? []

  if (!supportsEsc) {
    return (
      <div>
        <PageHeader
          eyebrow="Fieldbus"
          title="Diagnostics"
          description="Live link-quality and watchdog counters read from each slave's EtherCAT Slave Controller."
        />
        <div className="p-4 sm:px-8 sm:py-7 space-y-6">
          <NoEscNotice feature="ESC diagnostics" />
        </div>
      </div>
    )
  }

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
      <div className="p-4 sm:px-8 sm:py-7 space-y-6">
        <DiagnosticsExplainer />

        <div className="flex items-center justify-end gap-3">
          <WireTiming label="Diagnostics" timing={timing} />
          <button
            onClick={() => query.refetch()}
            disabled={query.isFetching}
            className={btnOutline}
          >
            {query.isFetching ? 'Loading…' : 'Refresh'}
          </button>
        </div>

        <CaptureSection />

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
