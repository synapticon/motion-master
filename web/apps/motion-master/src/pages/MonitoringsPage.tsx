import { useEffect, useMemo, useRef, useState } from 'react'
import { useMutation, useQueries, useQuery, useQueryClient } from '@tanstack/react-query'
import { adjectives, animals, colors, uniqueNamesGenerator } from 'unique-names-generator'
import { Eraser, Pause, Play, Plus, Trash2, X } from 'lucide-react'
import type uPlot from 'uplot'
import type { DeviceParameter, Monitoring } from '@synapticon/motion-master-client'
import MonitoringChart from '../components/MonitoringChart'
import PageHeader from '../components/PageHeader'
import ParameterPicker from '../components/ParameterPicker'
import SlavePositionBadge from '../components/SlavePositionBadge'
import { useConnection } from '../contexts/ConnectionContext'
import {
  MonitoringSocketProvider,
  type SampleRows,
  useMonitoringSocket,
} from '../contexts/MonitoringSocketContext'

const inputCls = 'border border-grey-300 px-3 py-2 text-sm w-full bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'
const btnCls =
  'bg-syn-red text-white px-4 py-2 text-xs hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'
const btnGhostCls =
  'border border-grey-300 text-grey-700 px-4 py-2 text-xs hover:bg-grey-50 disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

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
  return 'Unknown error'
}

function parseHexOrDec(s: string): number | null {
  const t = s.trim()
  if (/^0[xX][0-9a-fA-F]+$/.test(t)) return parseInt(t.slice(2), 16)
  if (/^[0-9]+$/.test(t)) return parseInt(t, 10)
  return null
}

function toHex(n: number, pad: number): string {
  return `0x${n.toString(16).toUpperCase().padStart(pad, '0')}`
}

function paramKey(pos: number, index: number, subindex: number): string {
  return `${pos}:${index}:${subindex}`
}

// CiA301 PDO mapping objects: RxPDO 0x1600–0x17FF, TxPDO 0x1A00–0x1BFF. Their sub-entries encode
// the process-image layout (which object is mapped where), not a measurable value — monitoring
// them is meaningless, and in a long list they're easily mistaken for real parameters. The picker
// shows them greyed-out and unselectable rather than offering them as a pick.
function isPdoMappingObject(p: DeviceParameter): boolean {
  return (p.index >= 0x1600 && p.index <= 0x17ff) || (p.index >= 0x1a00 && p.index <= 0x1bff)
}

interface CycleStats {
  mean: number
  median: number
  stdev: number
  min: number
  max: number
}

// Statistics of the cycle time — the Δt (µs) between consecutive samples — over the buffered
// window. Each sample carries its real cycle timestamp, so these reflect the true cycle period and
// its jitter (e.g. mean ≈ 1000 µs for a 1 ms loop), not the WebSocket flush cadence. stdev is the
// population standard deviation, the headline jitter figure.
function cycleStats(xs: number[]): CycleStats | null {
  if (xs.length < 2) return null
  let min = Infinity
  let max = -Infinity
  let sum = 0
  const deltas = new Array<number>(xs.length - 1)
  for (let i = 1; i < xs.length; i++) {
    const d = xs[i] - xs[i - 1]
    deltas[i - 1] = d
    if (d < min) min = d
    if (d > max) max = d
    sum += d
  }
  const mean = sum / deltas.length
  let sumSq = 0
  for (const d of deltas) sumSq += (d - mean) * (d - mean)
  const stdev = Math.sqrt(sumSq / deltas.length)
  const sorted = [...deltas].sort((a, b) => a - b)
  const mid = sorted.length >> 1
  const median = sorted.length % 2 === 1 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2
  return { mean, median, stdev, min, max }
}

function micros(v: number): string {
  return `${Math.round(v).toLocaleString()} µs`
}

// A memorable, URL-safe identifier + Title-cased label for a new monitoring, e.g. topic
// "swift-amber-otter", name "Swift Amber Otter". adjectives × colors × animals is tens of
// thousands of combos, so a re-roll comfortably resolves the rare clash with an existing topic.
function generateMonitoringName(): { topic: string; name: string } {
  const words = uniqueNamesGenerator({
    dictionaries: [adjectives, colors, animals],
    separator: ' ',
    style: 'capital',
  }).split(' ')
  return { topic: words.join('-').toLowerCase(), name: words.join(' ') }
}

// A device known to the bus (subset of the getDevices payload we use here).
interface DeviceInfo {
  slavePosition: number
  name: string
}

// One row in the create form's parameter list.
interface ParamRowState {
  devicePosition: string
  index: string
  subindex: string
}

const emptyRow: ParamRowState = { devicePosition: '', index: '', subindex: '' }

export default function MonitoringsPage() {
  const { api, hasScanned, host, wsPort } = useConnection()
  const wsUrl = `wss://${host}:${wsPort}`
  const queryClient = useQueryClient()

  const devicesQuery = useQuery({
    queryKey: ['devices'],
    queryFn: () => api.getDevices(),
    enabled: hasScanned,
  })
  const devices: DeviceInfo[] = devicesQuery.data?.data ?? []

  // Cached object dictionaries for every device, so we can resolve parameter names and offer
  // a pick-from-list in the form. Empty for devices whose OD has not been enumerated.
  const paramQueries = useQueries({
    queries: devices.map((d) => ({
      queryKey: ['deviceParameters', d.slavePosition],
      queryFn: () => api.getDeviceParameters(d.slavePosition),
      enabled: hasScanned,
    })),
  })
  const paramsByDevice = useMemo(() => {
    const map = new Map<number, DeviceParameter[]>()
    devices.forEach((d, i) => {
      map.set(d.slavePosition, paramQueries[i]?.data?.data ?? [])
    })
    return map
  }, [devices, paramQueries])
  const nameByKey = useMemo(() => {
    const map = new Map<string, string>()
    for (const [pos, params] of paramsByDevice) {
      for (const p of params) map.set(paramKey(pos, p.index, p.subindex), p.name)
    }
    return map
  }, [paramsByDevice])

  const monitoringsQuery = useQuery({
    queryKey: ['monitorings'],
    queryFn: () => api.listMonitorings(),
    enabled: hasScanned,
    refetchInterval: 5000, // keep the list (and per-parameter source classification) fresh
  })
  const monitorings: Monitoring[] = monitoringsQuery.data?.data ?? []

  const deleteMutation = useMutation({
    mutationFn: (topic: string) => api.deleteMonitoring(topic),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['monitorings'] }),
  })

  if (!hasScanned) {
    return (
      <div>
        <PageHeader eyebrow="Data" title="Monitorings" />
        <p className="px-8 py-6 text-sm text-grey-600">
          Connect and scan the bus to create and view monitorings.
        </p>
      </div>
    )
  }

  return (
    <div>
      <PageHeader
        eyebrow="Data"
        title="Monitorings"
        description="Record drive parameters over time. PDO-mapped objects are read live from the process image; others are polled over SDO. Monitoring collects only while a device is in SAFE-OP/OP."
      />

      <div className="px-8 py-6 space-y-8">
        <CreateMonitoringForm
          devices={devices}
          paramsByDevice={paramsByDevice}
          onCreated={() => queryClient.invalidateQueries({ queryKey: ['monitorings'] })}
        />

        {monitorings.length === 0 ? (
          <p className="text-sm text-grey-500">No monitorings yet. Create one above.</p>
        ) : (
          <MonitoringSocketProvider key={wsUrl} url={wsUrl}>
            <div className="space-y-4">
              {monitorings.map((m) => (
                <MonitoringCard
                  key={m.topic}
                  monitoring={m}
                  nameByKey={nameByKey}
                  onDelete={() => deleteMutation.mutate(m.topic)}
                  deleting={deleteMutation.isPending && deleteMutation.variables === m.topic}
                />
              ))}
            </div>
          </MonitoringSocketProvider>
        )}
      </div>
    </div>
  )
}

// --- Monitoring card ---------------------------------------------------------

function MonitoringCard({
  monitoring,
  nameByKey,
  onDelete,
  deleting,
}: {
  monitoring: Monitoring
  nameByKey: Map<string, string>
  onDelete: () => void
  deleting: boolean
}) {
  const { subscribe } = useMonitoringSocket()
  const params = monitoring.parameters
  const seriesCount = params.length

  const [playing, setPlaying] = useState(true)
  const playingRef = useRef(true)
  useEffect(() => {
    playingRef.current = playing
  }, [playing])

  const [retention, setRetention] = useState(10000)
  const retentionRef = useRef(10000)
  useEffect(() => {
    retentionRef.current = retention
  }, [retention])

  // Live buffer kept in refs (mutated in place for speed); a fresh outer tuple is pushed to
  // `data` state each batch so the chart's setData runs without copying the whole buffer.
  // x values are microseconds elapsed since the first sample (t0Ref), so the axis shows relative
  // time at the data's true resolution rather than coarse whole-second wall-clock ticks.
  const t0Ref = useRef<number | null>(null)
  const xsRef = useRef<number[]>([])
  const ysRef = useRef<(number | null)[][]>(params.map(() => []))
  const [data, setData] = useState<uPlot.AlignedData>(
    () => [[], ...params.map(() => [])] as uPlot.AlignedData,
  )
  const [count, setCount] = useState(0)
  // Cycle-time statistics over the buffer, recomputed at most ~4×/s (the median sort is O(N log N))
  // even though data streams faster.
  const [stats, setStats] = useState<CycleStats | null>(null)
  const statsAtRef = useRef(0)

  // Stable across list refetches (which hand back new param objects): keyed on the content
  // signature so the chart isn't torn down every poll. Names live in the table below.
  const sig = useMemo(
    () => params.map((p) => `${p.devicePosition}:${p.index}:${p.subindex}`).join('|'),
    [params],
  )
  const labels = useMemo(
    () => params.map((p) => `${p.devicePosition}·${toHex(p.index, 4)}:${toHex(p.subindex, 2).slice(2)}`),
    [sig],
  )
  // Parameter names, positionally aligned with `labels`, surfaced as the legend row's hover title
  // (the legend itself shows the compact device·index:subindex key). Empty when the name is unknown.
  const titles = useMemo(
    () => params.map((p) => nameByKey.get(paramKey(p.devicePosition, p.index, p.subindex)) ?? ''),
    [sig, nameByKey],
  )

  useEffect(() => {
    const onBatch = (rows: SampleRows) => {
      if (!playingRef.current) return
      for (const row of rows) {
        // row[0] is the cycle timestamp in epoch microseconds. Plot microseconds elapsed since the
        // first sample so the x-axis keeps the data's resolution (the chart formats it as µs/ms/s).
        const ts = typeof row[0] === 'number' ? row[0] : 0
        if (t0Ref.current === null) t0Ref.current = ts
        xsRef.current.push(ts - t0Ref.current)
        for (let i = 0; i < seriesCount; i++) {
          const v = row[i + 1]
          ysRef.current[i].push(typeof v === 'number' ? v : null)
        }
      }
      const overflow = xsRef.current.length - retentionRef.current
      if (overflow > 0) {
        xsRef.current.splice(0, overflow)
        for (const ys of ysRef.current) ys.splice(0, overflow)
      }
      setData([xsRef.current, ...ysRef.current] as uPlot.AlignedData)
      setCount(xsRef.current.length)
      // Recompute cycle-time stats at most ~4×/s — the median sort is O(N log N) and the buffer can
      // hold thousands of points, but the figures don't need per-batch updates.
      const nowMs = performance.now()
      if (nowMs - statsAtRef.current > 250) {
        statsAtRef.current = nowMs
        setStats(cycleStats(xsRef.current))
      }
    }
    return subscribe(monitoring.topic, onBatch)
  }, [subscribe, monitoring.topic, seriesCount])

  function clear() {
    t0Ref.current = null
    xsRef.current = []
    ysRef.current = params.map(() => [])
    setData([[], ...params.map(() => [])] as uPlot.AlignedData)
    setCount(0)
    setStats(null)
    statsAtRef.current = 0
  }

  return (
    <div className="border border-grey-200 bg-white">
      <div className="flex items-start justify-between px-5 py-3 border-b border-grey-100">
        <div>
          <h3 className="font-display text-lg">
            {monitoring.name ?? monitoring.topic}
            {monitoring.name && (
              <span className="ml-2 text-xs text-grey-400 font-mono">{monitoring.topic}</span>
            )}
          </h3>
          <p className="text-xs text-grey-500 mt-0.5">
            flush every {monitoring.interval} ms · lossless ({seriesCount}{' '}
            {seriesCount === 1 ? 'parameter' : 'parameters'})
          </p>
        </div>
        <button
          type="button"
          className={`${btnGhostCls} inline-flex items-center gap-1.5`}
          onClick={onDelete}
          disabled={deleting}
        >
          <Trash2 className="h-4 w-4" />
          {deleting ? 'Deleting…' : 'Delete'}
        </button>
      </div>

      <table className="w-full text-sm">
        <thead>
          <tr className="text-left text-xs text-grey-500 uppercase tracking-wide">
            <th className="px-5 py-2 font-medium">Slave</th>
            <th className="px-3 py-2 font-medium">Address</th>
            <th className="px-3 py-2 font-medium">Name</th>
            <th className="px-3 py-2 font-medium">Source</th>
          </tr>
        </thead>
        <tbody>
          {monitoring.parameters.map((p) => {
            const name = nameByKey.get(paramKey(p.devicePosition, p.index, p.subindex))
            return (
              <tr key={paramKey(p.devicePosition, p.index, p.subindex)} className="border-t border-grey-100">
                <td className="px-5 py-2">
                  <SlavePositionBadge position={p.devicePosition} />
                </td>
                <td className="px-3 py-2 font-mono text-xs">
                  {toHex(p.index, 4)}:{toHex(p.subindex, 2).slice(2)}
                </td>
                <td className="px-3 py-2 text-grey-700">{name ?? '—'}</td>
                <td className="px-3 py-2">
                  <SourceBadge source={p.source} />
                </td>
              </tr>
            )
          })}
        </tbody>
      </table>

      <div className="px-5 py-3 border-t border-grey-100">
        <div className="flex flex-wrap items-center gap-3 mb-3">
          <button
            type="button"
            className={`${btnGhostCls} inline-flex items-center gap-1.5`}
            onClick={() => {
              // Resuming: start fresh. No samples arrive while paused, so the first post-resume
              // sample is a full pause-duration ahead of the last one — that bridging Δt would
              // otherwise corrupt the trace and the cycle-time stats.
              if (!playing) {
                clear()
              }
              setPlaying((p) => !p)
            }}
          >
            {playing ? <Pause className="h-4 w-4" /> : <Play className="h-4 w-4" />}
            {playing ? 'Pause' : 'Resume'}
          </button>
          <button
            type="button"
            className={`${btnGhostCls} inline-flex items-center gap-1.5`}
            onClick={clear}
          >
            <Eraser className="h-4 w-4" />
            Clear
          </button>
          <label className="text-xs text-grey-500 flex items-center gap-1.5">
            Retain
            <input
              className="border border-grey-300 px-2 py-1 text-xs w-24 bg-white"
              value={retention}
              inputMode="numeric"
              onChange={(e) => {
                const n = Number(e.target.value)
                if (Number.isFinite(n) && n > 0) setRetention(Math.floor(n))
              }}
            />
            samples
          </label>
          <span className="text-xs text-grey-400 ml-auto">
            {count.toLocaleString()} / {retention.toLocaleString()} samples ·{' '}
            {playing ? 'live' : 'paused'}
          </span>
        </div>
        {stats && (
          <div className="mb-2 text-center text-xs text-grey-500 cursor-help" title="Cycle time — Δt between consecutive samples">
            <span className="uppercase tracking-wide text-grey-400">Cycle time</span>{' '}
            mean <span className="font-mono text-grey-700">{micros(stats.mean)}</span> · median{' '}
            <span className="font-mono text-grey-700">{micros(stats.median)}</span> · σ{' '}
            <span className="font-mono text-grey-700">{micros(stats.stdev)}</span> · min{' '}
            <span className="font-mono text-grey-700">{micros(stats.min)}</span> · max{' '}
            <span className="font-mono text-grey-700">{micros(stats.max)}</span>
          </div>
        )}
        <MonitoringChart data={data} labels={labels} titles={titles} />
      </div>
    </div>
  )
}

function SourceBadge({ source }: { source: string }) {
  const pdo = source === 'pdo'
  return (
    <span
      title={
        pdo
          ? 'PDO — decoded live from the process image each tick'
          : 'SDO — polled in the background and read from cache'
      }
      className={`px-1.5 py-0.5 rounded-sm text-[10px] font-display tracking-wider uppercase cursor-help ${pdo ? 'bg-ocean/10 text-ocean' : 'bg-grey-100 text-grey-600'
        }`}
    >
      {source}
    </span>
  )
}

// --- Create form -------------------------------------------------------------

function CreateMonitoringForm({
  devices,
  paramsByDevice,
  onCreated,
}: {
  devices: DeviceInfo[]
  paramsByDevice: Map<number, DeviceParameter[]>
  onCreated: () => void
}) {
  const { api } = useConnection()
  const [open, setOpen] = useState(false)
  const [name, setName] = useState('')
  const [topic, setTopic] = useState('')
  const [interval, setInterval] = useState('16')
  const [rows, setRows] = useState<ParamRowState[]>([{ ...emptyRow }])
  const [error, setError] = useState<string | null>(null)

  // Fill the topic + name with a fresh funny pair (both stay editable). Used to seed the form on
  // open and from the 🎲 button.
  function reroll() {
    const g = generateMonitoringName()
    setTopic(g.topic)
    setName(g.name)
  }

  const createMutation = useMutation({
    mutationFn: (body: {
      topic: string
      name?: string
      interval: number
      parameters: number[][]
    }) => api.createMonitoring(body),
    onSuccess: () => {
      setName('')
      setTopic('')
      setInterval('16')
      setRows([{ ...emptyRow }])
      setError(null)
      setOpen(false)
      onCreated()
    },
    onError: (err) => setError(apiError(err)),
  })

  function setRow(i: number, patch: Partial<ParamRowState>) {
    setRows((rs) => rs.map((r, j) => (j === i ? { ...r, ...patch } : r)))
  }

  function submit() {
    setError(null)
    if (!/^[A-Za-z0-9._-]{1,64}$/.test(topic)) {
      setError('Topic must be 1–64 chars of A–Z, a–z, 0–9, . _ -')
      return
    }
    const intervalMs = Number(interval)
    if (!Number.isInteger(intervalMs) || intervalMs < 5 || intervalMs > 2000) {
      setError('Interval must be an integer between 5 and 2000 ms')
      return
    }
    const parameters: number[][] = []
    for (const [i, r] of rows.entries()) {
      const pos = parseHexOrDec(r.devicePosition)
      const index = parseHexOrDec(r.index)
      const subindex = parseHexOrDec(r.subindex)
      if (pos === null || index === null || subindex === null) {
        setError(`Parameter ${i + 1}: device, index and subindex are required`)
        return
      }
      parameters.push([pos, index, subindex])
    }
    if (parameters.length === 0) {
      setError('Add at least one parameter')
      return
    }
    createMutation.mutate({
      topic,
      name: name.trim() === '' ? undefined : name.trim(),
      interval: intervalMs,
      parameters,
    })
  }

  if (!open) {
    return (
      <button
        type="button"
        className={btnCls}
        onClick={() => {
          reroll() // seed a funny topic + name so creating one is a single click
          setOpen(true)
        }}
      >
        New monitoring
      </button>
    )
  }

  return (
    <div className="border border-grey-200 bg-white p-5 space-y-4">
      <div className="flex items-center justify-between">
        <span className="text-xs text-grey-500 uppercase tracking-wide">New monitoring</span>
        <button
          type="button"
          className={`${btnGhostCls} px-3`}
          onClick={reroll}
          title="Generate a random topic and name"
        >
          🎲 Random name
        </button>
      </div>
      <div className="grid grid-cols-2 gap-4 md:grid-cols-3">
        <div>
          <label className={labelCls}>Topic</label>
          <input className={inputCls} value={topic} onChange={(e) => setTopic(e.target.value)} placeholder="left-leg" />
        </div>
        <div>
          <label className={labelCls}>Name (optional)</label>
          <input className={inputCls} value={name} onChange={(e) => setName(e.target.value)} placeholder="Left Leg" />
        </div>
        <div>
          <label className={labelCls}>Flush interval (ms)</label>
          <input className={inputCls} value={interval} onChange={(e) => setInterval(e.target.value)} inputMode="numeric" />
          <p className="mt-1 text-[10px] text-grey-400">
            5–2000 ms (16 ms ≈ one batch per 60 Hz frame). How often a batch is sent; the stream is
            lossless (every cycle), so this trades message size against frequency, not resolution.
          </p>
        </div>
      </div>

      <div>
        <label className={labelCls}>Parameters</label>
        <div className="space-y-2">
          {rows.map((row, i) => (
            <ParamRow
              key={i}
              row={row}
              devices={devices}
              paramsByDevice={paramsByDevice}
              onChange={(patch) => setRow(i, patch)}
              onRemove={rows.length > 1 ? () => setRows((rs) => rs.filter((_, j) => j !== i)) : undefined}
            />
          ))}
        </div>
        <button
          type="button"
          className={`${btnGhostCls} mt-2 inline-flex items-center gap-1.5`}
          onClick={() =>
            setRows((rs) => [
              // Carry the previous row's device forward — appended parameters are usually for the
              // same device, so this saves re-picking it every time.
              ...rs,
              { ...emptyRow, devicePosition: rs[rs.length - 1]?.devicePosition ?? '' },
            ])
          }
        >
          <Plus className="h-4 w-4" />
          Append parameter
        </button>
      </div>

      {error && <p className="text-sm text-syn-red">{error}</p>}

      <div className="flex gap-2">
        <button type="button" className={btnCls} onClick={submit} disabled={createMutation.isPending}>
          {createMutation.isPending ? 'Creating…' : 'Create'}
        </button>
        <button type="button" className={btnGhostCls} onClick={() => setOpen(false)}>
          Cancel
        </button>
      </div>
    </div>
  )
}

function ParamRow({
  row,
  devices,
  paramsByDevice,
  onChange,
  onRemove,
}: {
  row: ParamRowState
  devices: DeviceInfo[]
  paramsByDevice: Map<number, DeviceParameter[]>
  onChange: (patch: Partial<ParamRowState>) => void
  onRemove?: () => void
}) {
  const pos = parseHexOrDec(row.devicePosition)
  const deviceParams = pos !== null ? (paramsByDevice.get(pos) ?? []) : []

  return (
    <div className="flex flex-wrap items-end gap-2">
      <div className="w-40">
        <span className="block text-[10px] text-grey-400 mb-0.5">Device</span>
        <select
          className={inputCls}
          value={row.devicePosition}
          onChange={(e) => onChange({ devicePosition: e.target.value })}
        >
          <option value="">Select device…</option>
          {devices.map((d) => (
            <option key={d.slavePosition} value={String(d.slavePosition)}>
              {d.slavePosition} — {d.name || 'device'}
            </option>
          ))}
        </select>
      </div>

      {pos !== null && (
        <div className="w-[32rem]">
          <span className="block text-[10px] text-grey-400 mb-0.5">Pick parameter</span>
          <ParameterPicker
            className="w-[32rem]"
            params={deviceParams}
            selectedIndex={parseHexOrDec(row.index)}
            selectedSubindex={parseHexOrDec(row.subindex)}
            onSelect={(p) => onChange({ index: String(p.index), subindex: String(p.subindex) })}
            isDisabled={isPdoMappingObject}
            disabledHint="PDO mapping"
          />
        </div>
      )}

      <div className="w-28">
        <span className="block text-[10px] text-grey-400 mb-0.5">Index</span>
        <input
          className={inputCls}
          value={row.index}
          onChange={(e) => onChange({ index: e.target.value })}
          placeholder="0x6064"
        />
      </div>
      <div className="w-24">
        <span className="block text-[10px] text-grey-400 mb-0.5">Subindex</span>
        <input
          className={inputCls}
          value={row.subindex}
          onChange={(e) => onChange({ subindex: e.target.value })}
          placeholder="0"
        />
      </div>

      {onRemove && (
        <button
          type="button"
          className="flex items-center justify-center border border-grey-300 px-3 py-2.5 text-syn-red hover:bg-syn-red hover:text-white cursor-pointer transition-colors"
          onClick={onRemove}
          title="Remove parameter"
        >
          <X className="h-4 w-4" />
        </button>
      )}
    </div>
  )
}
