import { useEffect, useMemo, useRef, useState } from 'react'
import { useParams } from 'react-router'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type uPlot from 'uplot'
import DevicePageHeader from '../components/DevicePageHeader'
import MonitoringChart from '../components/MonitoringChart'
import { useMonitoringSocket, type SampleRows } from '../contexts/MonitoringSocketContext'
import { useConnection } from '../contexts/ConnectionContext'
import { cia402StatusKey, useCia402Status } from '../hooks/useCia402Status'
import { useOperationModes } from '../hooks/useOperationModes'

// One shared control height for every interactive control — selects, inputs, and buttons — so a
// button and its neighbouring input/select are always the exact same height, and standalone
// buttons match too. Explicit px (not a spacing-scale step): this theme's spacing scale is
// geometric (h-9 = 6rem = 96px!), so a numeric height utility would NOT be ~36px — see theme.css.
const inputCls = 'border border-grey-300 px-3 h-[38px] text-sm w-full bg-white'
// Buttons share the h-[38px] control height (above) and are inline-flex + centred so the label
// stays vertically centred at that fixed height. They also never wrap or shrink: a two-word label
// ("Set mode", "Quick stop", "Reset fault") would otherwise break in two as its row narrowed —
// beside a flex-1 select in the Mode card, and in the Operation card's wrapping button row.
const btnCls =
  'inline-flex shrink-0 items-center justify-center whitespace-nowrap bg-syn-red text-white px-4 ' +
  'h-[38px] text-xs hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed ' +
  'cursor-pointer transition-colors'
const btnGhostCls =
  'inline-flex shrink-0 items-center justify-center whitespace-nowrap border border-grey-300 ' +
  'text-grey-700 px-4 h-[38px] text-xs hover:bg-grey-50 disabled:opacity-50 ' +
  'disabled:cursor-not-allowed cursor-pointer ' +
  'transition-colors'

type Quantity = 'position' | 'velocity' | 'torque'

// Synapticon's established per-quantity colours — the same ones used across the product for the
// actual-value objects (0x6064 position, 0x606C velocity, 0x6077 torque). Reused here so a quantity
// reads the same on the Motion page (graph trace + toggle button) as everywhere else in the suite.
const QUANTITY_COLOR: Record<Quantity, string> = {
  position: '#B82698', // 0x6064:00 position actual value
  velocity: '#A2CD39', // 0x606C:00 velocity actual value
  torque: '#FFDD55', // 0x6077:00 torque actual value
}

// Black or white text for legibility on a given background — the green/yellow quantity colours need
// dark text, the magenta needs white. Uses the standard perceived-luminance weighting.
function readableTextOn(hex: string): string {
  const r = parseInt(hex.slice(1, 3), 16)
  const g = parseInt(hex.slice(3, 5), 16)
  const b = parseInt(hex.slice(5, 7), 16)
  const luminance = (0.299 * r + 0.587 * g + 0.114 * b) / 255
  return luminance > 0.6 ? '#1a1a1a' : '#ffffff'
}

// A drive follows a single setpoint at a time, chosen by its active mode: position in PP/CSP,
// velocity in PV/CSV, torque in PT/CST. Homing and No-mode have no linear setpoint to command.
function targetKindForMode(mode: number): Quantity | null {
  switch (mode) {
    case 1:
    case 8:
      return 'position'
    case 3:
    case 9:
      return 'velocity'
    case 4:
    case 10:
      return 'torque'
    default:
      return null
  }
}

// The CiA402 object each (quantity, role) pair reads/writes — used to build the live monitoring.
// target position 0x607A / position actual 0x6064; target velocity 0x60FF / velocity actual 0x606C;
// target torque 0x6071 / torque actual 0x6077. All subindex 0.
type SeriesRole = 'target' | 'actual'
interface SeriesDef {
  quantity: Quantity
  role: SeriesRole
  index: number
}
const SERIES: SeriesDef[] = [
  { quantity: 'position', role: 'target', index: 0x607a },
  { quantity: 'position', role: 'actual', index: 0x6064 },
  { quantity: 'velocity', role: 'target', index: 0x60ff },
  { quantity: 'velocity', role: 'actual', index: 0x606c },
  { quantity: 'torque', role: 'target', index: 0x6071 },
  { quantity: 'torque', role: 'actual', index: 0x6077 },
]

// Surfaces the node layer's error string from a failed request (matching the other device pages).
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
      const { status } = err as { status: number }
      return `HTTP ${status}`
    }
  }
  if (err instanceof Error) return err.message
  return 'Unknown error'
}

// A thrown request carries the HTTP status; 409 from createMonitoring means the topic already
// exists (so we reuse it rather than surfacing an error).
function isConflict(err: unknown): boolean {
  return !!err && typeof err === 'object' && (err as { status?: number }).status === 409
}

export default function DeviceMotionPage() {
  const { deviceId } = useParams()
  const slavePosition = Number(deviceId)
  const { api } = useConnection()
  const queryClient = useQueryClient()

  // The same query the sticky status bar reads, through the shared hook — this page reads the
  // drive's state to decide which setpoint is live and to seed its inputs; the bar displays it.
  const statusKey = cia402StatusKey(slavePosition)
  const statusQuery = useCia402Status(slavePosition)
  const status = statusQuery.data
  const activeMode = status?.modeOfOperation ?? 0

  // What the drive itself says it has, rather than a list written here. Split into the two groups
  // the dropdown shows: CiA402's modes, and the vendor's negative ones.
  const modesQuery = useOperationModes(slavePosition)
  const standardModes = (modesQuery.data?.modes ?? []).filter((m) => m.kind === 'standard')
  const manufacturerModes = (modesQuery.data?.modes ?? []).filter((m) => m.kind === 'manufacturer')

  // The mode dropdown selection. Seed it from the drive's active mode on first load, then it is the
  // user's to change until they press "Set mode".
  const [selectedMode, setSelectedMode] = useState<number | null>(null)
  useEffect(() => {
    if (selectedMode === null && status) setSelectedMode(status.modeOfOperation)
  }, [status, selectedMode])

  const setModeMutation = useMutation({
    mutationFn: (mode: number) => api.setCia402OperationMode(slavePosition, { mode }),
    onSuccess: (r) => queryClient.setQueryData(statusKey, r.data),
  })

  const commandMutation = useMutation({
    mutationFn: (command: 'enable' | 'disable' | 'quickStop' | 'faultReset') =>
      api.runCia402Command(slavePosition, { command }),
    onSuccess: (r) => queryClient.setQueryData(statusKey, r.data),
  })

  // The target write is tied to the drive's ACTIVE mode (0x6061), not the dropdown selection — you
  // command the setpoint for the mode the drive is actually in.
  const targetKind = targetKindForMode(activeMode)
  // Seed the target input from the drive's current setpoint once (0x607A/0x60FF/0x6071 for the
  // active mode, surfaced as status.target), then it is the user's to edit — the same seed-once
  // pattern as selectedMode above. Not live-tracked: an input that rewrites itself as you type
  // would be hostile.
  const [targetValue, setTargetValue] = useState<string | null>(null)
  useEffect(() => {
    if (targetValue === null && status) setTargetValue(String(status.target))
  }, [status, targetValue])
  const targetMutation = useMutation({
    mutationFn: ({ target, value }: { target: Quantity; value: number }) =>
      api.setCia402Target(slavePosition, { target, value }),
  })

  return (
    <div>
      <DevicePageHeader
        slavePosition={slavePosition}
        title="Motion"
        description="Drive the CiA402 state machine, operation mode, and cyclic setpoints of this drive, and watch target vs. actual live."
      />
      <div className="p-4 sm:px-8 sm:py-7 space-y-6">
        {/* Row 1 — the three control cards (Mode, Operation, Target) across the top; the live
            graph gets its own full-width row below. Cards stretch to equal height (default grid
            align — no items-start); each is a flex column with its title + blurb on top and the
            controls pinned to the bottom (mt-auto) so the action rows line up across cards. */}
        <section>
          <div className="grid grid-cols-1 lg:grid-cols-3 gap-6">
            {/* Mode — chosen first; it decides which setpoint is active. */}
            <div className="border border-grey-200 p-5 flex flex-col">
              <div className="space-y-2">
                <h3 className="text-sm font-display uppercase">Mode</h3>
                <p className="text-xs text-grey-600">
                  Request an operation mode (0x6060). The drive adopts it — reflected in the status
                  bar above as the active mode (0x6061) — once accepted. Change the mode with the
                  drive disabled; the mode decides which setpoint is active. The list is the drive's
                  own: a standard mode it does not advertise in 0x6502 is shown but cannot be
                  selected, and the manufacturer modes are listed because the drive has no way to
                  advertise those either way.
                </p>
              </div>
              <div className="mt-auto pt-4 space-y-3">
                <div className="flex items-center gap-3">
                  <div className="flex-1 min-w-0">
                    <select
                      id="mode-select"
                      aria-label="Operation mode"
                      className={inputCls}
                      value={selectedMode ?? activeMode}
                      onChange={(e) => setSelectedMode(Number(e.target.value))}
                    >
                      {/* Unsupported modes stay in the list, disabled: seeing that this drive does
                          not do `ip` is worth more than a shorter list you cannot ask about. */}
                      <optgroup label="Standard">
                        {standardModes.map((m) => (
                          <option key={m.value} value={m.value} disabled={m.supported === false}>
                            {m.label} ({m.value}){m.supported === false ? ' — not supported' : ''}
                          </option>
                        ))}
                      </optgroup>
                      {manufacturerModes.length > 0 && (
                        <optgroup label="Manufacturer-specific">
                          {manufacturerModes.map((m) => (
                            <option key={m.value} value={m.value}>
                              {m.label} ({m.value}){m.deprecated ? ' — deprecated' : ''}
                            </option>
                          ))}
                        </optgroup>
                      )}
                    </select>
                  </div>
                  <button
                    className={btnCls}
                    disabled={setModeMutation.isPending || selectedMode === null}
                    onClick={() => selectedMode !== null && setModeMutation.mutate(selectedMode)}
                  >
                    Set mode
                  </button>
                </div>
                {setModeMutation.isError && (
                  <p className="text-status-bad text-xs">{apiError(setModeMutation.error)}</p>
                )}
              </div>
            </div>

            {/* Operation — step the CiA402 device-control state machine. */}
            <div className="border border-grey-200 p-5 flex flex-col">
              <div className="space-y-2">
                <h3 className="text-sm font-display uppercase">Operation</h3>
                <p className="text-xs text-grey-600">
                  Step the CiA402 device-control state machine. <strong>Enable</strong> walks every
                  transition up to Operation Enabled (clearing a fault first if needed);{' '}
                  <strong>Disable</strong> returns to Switch On Disabled;{' '}
                  <strong>Quick stop</strong> triggers a controlled quick stop;{' '}
                  <strong>Reset fault</strong> clears a latched fault.
                </p>
              </div>
              <div className="mt-auto pt-4 space-y-3">
                <div className="flex flex-wrap gap-2">
                  <button
                    className={btnCls}
                    disabled={commandMutation.isPending}
                    onClick={() => commandMutation.mutate('enable')}
                  >
                    Enable
                  </button>
                  <button
                    className={btnGhostCls}
                    disabled={commandMutation.isPending}
                    onClick={() => commandMutation.mutate('disable')}
                  >
                    Disable
                  </button>
                  <button
                    className={btnGhostCls}
                    disabled={commandMutation.isPending}
                    onClick={() => commandMutation.mutate('quickStop')}
                  >
                    Quick stop
                  </button>
                  <button
                    className={btnGhostCls}
                    disabled={commandMutation.isPending}
                    onClick={() => commandMutation.mutate('faultReset')}
                  >
                    Reset fault
                  </button>
                </div>
                {commandMutation.isError && (
                  <p className="text-status-bad text-xs">{apiError(commandMutation.error)}</p>
                )}
              </div>
            </div>

            {/* Target — the cyclic setpoint for the active mode (the kind is in the card title). */}
            <div className="border border-grey-200 p-5 flex flex-col">
              <div className="space-y-2">
                <h3 className="text-sm font-display uppercase">
                  {targetKind ? `Target ${targetKind}` : 'Target'}
                </h3>
                <p className="text-xs text-grey-600">
                  Write the cyclic setpoint for the active mode
                  {targetKind ? (
                    <>
                      {' '}
                      — <strong>target {targetKind}</strong>. Values are signed (negative
                      reverses). No engineering units yet: this is the raw object value.
                    </>
                  ) : (
                    <> — the active mode ({status?.modeName ?? '—'}) has no linear setpoint.</>
                  )}
                </p>
              </div>
              <div className="mt-auto pt-4 space-y-3">
                <div className="flex items-center gap-3">
                  <div className="flex-1 min-w-0">
                    <input
                      id="target-input"
                      aria-label={targetKind ? `Target ${targetKind}` : 'Target value'}
                      className={inputCls}
                      type="number"
                      value={targetValue ?? ''}
                      onChange={(e) => setTargetValue(e.target.value)}
                      disabled={!targetKind}
                    />
                  </div>
                  <button
                    className={btnCls}
                    disabled={
                      !targetKind || targetMutation.isPending || (targetValue ?? '').trim() === ''
                    }
                    onClick={() => {
                      const value = Number(targetValue)
                      if (targetKind && Number.isFinite(value)) {
                        targetMutation.mutate({ target: targetKind, value: Math.trunc(value) })
                      }
                    }}
                  >
                    Set target
                  </button>
                </div>
                {targetMutation.isError && (
                  <p className="text-status-bad text-xs">{apiError(targetMutation.error)}</p>
                )}
              </div>
            </div>
          </div>
        </section>

        {/* Live — its own full-width row; the chart wants the space. */}
        <section>
          <div className="border border-grey-200 p-5 space-y-4">
            <h3 className="text-sm font-display uppercase">Live</h3>
            <p className="text-xs text-grey-600 max-w-2xl">
              Target vs. actual, streamed from a monitoring created for this device. Choose which
              quantity to plot.
            </p>
            <MotionGraph slavePosition={slavePosition} defaultQuantity={targetKind ?? 'position'} />
          </div>
        </section>
      </div>
    </div>
  )
}

// Creates a monitoring for this device's target+actual objects (whichever exist in its OD), streams
// it over the monitoring WebSocket, and plots the target-vs-actual pair for the selected quantity on
// a shared time axis. The monitoring is torn down when the page unmounts.
function MotionGraph({
  slavePosition,
  defaultQuantity,
}: {
  slavePosition: number
  defaultQuantity: Quantity
}) {
  const { api } = useConnection()
  const { subscribe } = useMonitoringSocket()
  const topic = `motion-${slavePosition}`

  // Only monitor objects the device actually has — a CiA402 drive without, say, a torque object
  // would otherwise fail the whole monitoring create. Reuse the shared OD cache the sidebar fills.
  const paramsQuery = useQuery({
    queryKey: ['deviceParameters', slavePosition],
    queryFn: () => api.getDeviceParameters(slavePosition),
    staleTime: Infinity,
  })
  const odIndices = useMemo(() => {
    const set = new Set<number>()
    for (const p of paramsQuery.data?.data ?? []) {
      if (p.subindex === 0) set.add(p.index)
    }
    return set
  }, [paramsQuery.data])

  // The series that exist on this device, in a stable order → column i occupies row[i + 1].
  const series = useMemo(
    () => SERIES.filter((s) => odIndices.has(s.index)),
    [odIndices],
  )
  const available: Quantity[] = useMemo(() => {
    const q = new Set<Quantity>()
    for (const s of series) {
      // A quantity is plottable only when both its target and actual object are present.
      if (series.some((t) => t.quantity === s.quantity && t.role !== s.role)) q.add(s.quantity)
    }
    return (['position', 'velocity', 'torque'] as Quantity[]).filter((x) => q.has(x))
  }, [series])

  const [quantity, setQuantity] = useState<Quantity>(defaultQuantity)
  // Fall back to a plottable quantity once the OD is known.
  useEffect(() => {
    if (available.length > 0 && !available.includes(quantity)) setQuantity(available[0])
  }, [available, quantity])

  // Create the monitoring once the OD is loaded. It is NOT deleted on leave — it persists as a
  // normal monitoring (visible on the Monitorings page) and is simply reused on a later visit: a
  // 409 means it already exists (a prior visit, another tab, or React StrictMode's dev double-mount)
  // and the topic + parameters are deterministic per device, so we adopt it rather than recreating.
  // Persisting also sidesteps the delete-on-unmount race the double-mount used to cause.
  const [createError, setCreateError] = useState<string | null>(null)
  const [ready, setReady] = useState(false)
  const seriesRef = useRef(series)
  seriesRef.current = series
  useEffect(() => {
    if (paramsQuery.isPending || series.length === 0) return
    let cancelled = false
    const parameters = series.map((s) => [slavePosition, s.index, 0])
    ;(async () => {
      try {
        await api.createMonitoring({ topic, name: `Motion ${slavePosition}`, interval: 16, parameters })
      } catch (err) {
        if (!isConflict(err)) {
          if (!cancelled) setCreateError(apiError(err))
          return
        }
      }
      if (!cancelled) {
        setReady(true)
        setCreateError(null)
      }
    })()
    return () => {
      cancelled = true
      setReady(false)
    }
    // series is stable unless the OD changes; recreate the monitoring if it does.
  }, [api, topic, slavePosition, series, paramsQuery.isPending])

  // Streaming buffer: one y-series per monitored object, x in µs elapsed since the first sample.
  const RETENTION = 4000
  const t0Ref = useRef<number | null>(null)
  const xsRef = useRef<number[]>([])
  const ysRef = useRef<(number | null)[][]>([])
  const [data, setData] = useState<uPlot.AlignedData>([[]] as unknown as uPlot.AlignedData)

  // Reset the buffer whenever the series set changes (column meaning changes).
  useEffect(() => {
    t0Ref.current = null
    xsRef.current = []
    ysRef.current = series.map(() => [])
    setData([[], ...series.map(() => [])] as uPlot.AlignedData)
  }, [series])

  useEffect(() => {
    if (!ready) return
    const onBatch = (rows: SampleRows) => {
      const n = seriesRef.current.length
      for (const row of rows) {
        const ts = typeof row[0] === 'number' ? row[0] : 0
        if (t0Ref.current === null) t0Ref.current = ts
        xsRef.current.push(ts - t0Ref.current)
        for (let i = 0; i < n; i++) {
          const v = row[i + 1]
          ysRef.current[i]?.push(typeof v === 'number' ? v : null)
        }
      }
      const overflow = xsRef.current.length - RETENTION
      if (overflow > 0) {
        xsRef.current.splice(0, overflow)
        for (const ys of ysRef.current) ys.splice(0, overflow)
      }
      setData([xsRef.current, ...ysRef.current] as uPlot.AlignedData)
    }
    return subscribe(topic, onBatch)
  }, [ready, subscribe, topic])

  // Project the two columns (target, actual) for the selected quantity out of the full buffer. A
  // fresh array each batch is fine: MonitoringChart keys its canvas rebuild on label content, not
  // array identity, so it streams via setData without tearing down.
  const view = useMemo(() => {
    const targetIdx = series.findIndex((s) => s.quantity === quantity && s.role === 'target')
    const actualIdx = series.findIndex((s) => s.quantity === quantity && s.role === 'actual')
    const xs = (data[0] ?? []) as number[]
    const cols: (number | null)[][] = []
    const labels: string[] = []
    if (targetIdx >= 0) {
      cols.push((data[targetIdx + 1] ?? []) as (number | null)[])
      labels.push('Target')
    }
    if (actualIdx >= 0) {
      cols.push((data[actualIdx + 1] ?? []) as (number | null)[])
      labels.push('Actual')
    }
    return { data: [xs, ...cols] as uPlot.AlignedData, labels }
  }, [data, series, quantity])

  if (paramsQuery.isPending) {
    return <p className="text-sm text-grey-500">Reading object dictionary…</p>
  }
  if (available.length === 0) {
    return (
      <p className="text-sm text-grey-500">
        This drive exposes no target/actual object pairs to plot.
      </p>
    )
  }

  // The visible "actual" trace carries the quantity's Synapticon colour; the (hidden-by-default)
  // target trace is a neutral grey so the recognisable colour belongs to the measured value.
  const traceColors = view.labels.map((l) => (l === 'Actual' ? QUANTITY_COLOR[quantity] : '#9ca3af'))

  return (
    <div className="space-y-3">
      <div className="flex gap-1">
        {available.map((q) => {
          const color = QUANTITY_COLOR[q]
          const active = q === quantity
          return (
            <button
              key={q}
              className="border px-3 py-1.5 text-xs uppercase cursor-pointer transition-colors"
              // The active toggle fills with the quantity's colour; an inactive one is the default
              // white-with-grey-border control.
              style={
                active
                  ? { backgroundColor: color, borderColor: color, color: readableTextOn(color) }
                  : { backgroundColor: 'white', borderColor: '#d1d5db', color: '#374151' }
              }
              onClick={() => setQuantity(q)}
            >
              {q}
            </button>
          )
        })}
      </div>
      {createError ? (
        <p className="text-sm text-syn-red">{createError}</p>
      ) : !ready ? (
        <p className="text-sm text-grey-500">Starting live stream…</p>
      ) : (
        <MonitoringChart
          data={view.data}
          labels={view.labels}
          hidden={['Target']}
          colors={traceColors}
        />
      )}
    </div>
  )
}
