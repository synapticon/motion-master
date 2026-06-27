import { useMemo, useRef, useState } from 'react'
import { useMutation } from '@tanstack/react-query'
import type uPlot from 'uplot'
import {
  fetchProcessDataDump,
  parseMmpd,
  isPlottableDataType,
  formatHex,
  type MmpdFile,
  type MmpdPdoEntry,
} from '@synapticon/motion-master-client'
import PageHeader from '../components/PageHeader'
import RecorderExplainer from '../components/RecorderExplainer'
import MonitoringChart from '../components/MonitoringChart'
import CycleStatsBar from '../components/CycleStatsBar'
import SlavePositionBadge from '../components/SlavePositionBadge'
import { useConnection } from '../contexts/ConnectionContext'
import { cycleStats } from '../utils/cycleStats'
import { btnOutline } from '../utils/styles'

// Pulls the server's { error } message out of a thrown request, for inline display.
function apiError(err: unknown): string {
  if (err && typeof err === 'object' && 'error' in err) {
    const inner = (err as { error: unknown }).error
    if (inner && typeof inner === 'object' && 'error' in inner) {
      return String((inner as { error: unknown }).error)
    }
  }
  return 'Failed to write the dump.'
}

// One PDO object flattened across devices, for the series picker.
interface FlatEntry {
  key: string
  devicePosition: number
  deviceName: string
  entry: MmpdPdoEntry
  plottable: boolean
}

function flatten(file: MmpdFile): FlatEntry[] {
  const out: FlatEntry[] = []
  for (const device of file.header.devices) {
    for (const entry of device.entries) {
      out.push({
        key: `${device.slavePosition}:${entry.index}:${entry.subindex}:${entry.isOutput ? 'o' : 'i'}`,
        devicePosition: device.slavePosition,
        deviceName: device.name,
        entry,
        plottable: isPlottableDataType(entry.dataType),
      })
    }
  }
  return out
}

const errorText = (e: unknown): string => (e instanceof Error ? e.message : String(e))

// One object's checkbox row in the series picker (direction is conveyed by the column it sits in).
function EntryCheckbox({
  item,
  checked,
  onToggle,
}: {
  item: FlatEntry
  checked: boolean
  onToggle: () => void
}) {
  return (
    <label
      className={`flex items-center gap-2 py-0.5 text-[11px] ${
        item.plottable ? 'cursor-pointer' : 'text-grey-400 cursor-not-allowed'
      }`}
      title={item.plottable ? item.entry.name : 'Not a plottable numeric type'}
    >
      <input type="checkbox" disabled={!item.plottable} checked={checked} onChange={onToggle} />
      <span className="font-mono">
        {formatHex(item.entry.index, 4)}:{formatHex(item.entry.subindex, 2, false)}
      </span>
      <span className="truncate">{item.entry.name}</span>
    </label>
  )
}

export default function RecorderPage() {
  const { api, host, httpPort } = useConnection()

  const [file, setFile] = useState<MmpdFile | null>(null)
  const [source, setSource] = useState('')
  const [selected, setSelected] = useState<Set<string>>(new Set())
  const [viewError, setViewError] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)
  const fileInputRef = useRef<HTMLInputElement>(null)

  // POST → writes a file on the server and returns its path (for terminal/headless users).
  const dumpMutation = useMutation({ mutationFn: () => api.dumpProcessData() })

  function load(parsed: MmpdFile, src: string) {
    setFile(parsed)
    setSource(src)
    setViewError(null)
    // Auto-select the first plottable object so the chart isn't empty.
    const first = flatten(parsed).find((e) => e.plottable)
    setSelected(new Set(first ? [first.key] : []))
  }

  async function recordAndView() {
    setLoading(true)
    setViewError(null)
    try {
      const parsed = await fetchProcessDataDump(`https://${host}:${httpPort}`)
      load(parsed, `Recorder ring — ${parsed.rowCount.toLocaleString()} cycles`)
    } catch (e) {
      setViewError(errorText(e))
    } finally {
      setLoading(false)
    }
  }

  async function onPickFile(e: React.ChangeEvent<HTMLInputElement>) {
    const picked = e.target.files?.[0]
    e.target.value = '' // let the same file be re-picked
    if (!picked) return
    setLoading(true)
    setViewError(null)
    try {
      const parsed = parseMmpd(await picked.arrayBuffer())
      load(parsed, `${picked.name} — ${parsed.rowCount.toLocaleString()} cycles`)
    } catch (e) {
      setViewError(errorText(e))
    } finally {
      setLoading(false)
    }
  }

  const flat = useMemo(() => (file ? flatten(file) : []), [file])

  // uPlot data for the selected series: x is microseconds elapsed since the first cycle (matching
  // the live MonitoringChart), then one decoded value array per chosen object.
  const chart = useMemo(() => {
    const empty = { data: [[]] as unknown as uPlot.AlignedData, labels: [] as string[], titles: [] as string[] }
    if (!file) return empty
    const chosen = flat.filter((e) => selected.has(e.key) && e.plottable)
    if (chosen.length === 0) return empty
    const tsUs = file.timestampsUs()
    const t0 = tsUs.length > 0 ? tsUs[0] : 0
    const xs = Array.from(tsUs, (t) => t - t0)
    const ys = chosen.map((e) => file.decodeSeries(e.entry))
    return {
      data: [xs, ...ys] as uPlot.AlignedData,
      labels: chosen.map(
        (e) => `${e.devicePosition}·${formatHex(e.entry.index, 4)}:${formatHex(e.entry.subindex, 2, false)}`,
      ),
      titles: chosen.map((e) => e.entry.name || ''),
    }
  }, [file, flat, selected])

  // Cycle-time stats over the recording's own timestamps — same jitter readout as the live
  // monitoring chart. Independent of which series are picked, so it reflects the recorded loop.
  const stats = useMemo(() => (file ? cycleStats(file.timestampsUs()) : null), [file])

  function toggle(key: string) {
    setSelected((prev) => {
      const next = new Set(prev)
      if (next.has(key)) {
        next.delete(key)
      } else {
        next.add(key)
      }
      return next
    })
  }

  // Group the picker by device, preserving image order.
  const byDevice = useMemo(() => {
    const groups: { position: number; name: string; outputs: FlatEntry[]; inputs: FlatEntry[] }[] = []
    for (const e of flat) {
      let g = groups.find((x) => x.position === e.devicePosition)
      if (!g) {
        g = { position: e.devicePosition, name: e.deviceName, outputs: [], inputs: [] }
        groups.push(g)
      }
      ;(e.entry.isOutput ? g.outputs : g.inputs).push(e)
    }
    return groups
  }, [flat])

  return (
    <div>
      <PageHeader
        eyebrow="Data"
        title="Recorder"
        description={
          <>
            The lossless process-data recorder captures every cyclic exchange into a circular ring
            held in memory — full raw inputs and outputs, an epoch-nanosecond timestamp, and the
            working counter for each cycle. It is the source for the live monitoring stream and for
            the dumps below.
          </>
        }
      />
      <div className="p-4 sm:p-8 space-y-8">
        <RecorderExplainer />

        <div className="border border-grey-200 px-4 py-3 max-w-2xl">
          <p className="eyebrow mb-1">Recorder dump</p>
          <p className="text-[11px] leading-snug text-grey-600">
            A <span className="font-mono">.mmpd</span> dump is every process-data cycle currently in
            the ring — full raw inputs and outputs, with the current process-image layout embedded
            as a header — captured oldest→newest at the moment you act (works while exchanging too)
            and decodable fully offline. <span className="font-medium">Record &amp; view</span>{' '}
            streams it here and plots it; <span className="font-medium">Open .mmpd file</span> loads
            one you saved earlier; <span className="font-medium">Dump to disk</span> writes it on the
            machine running Motion Master (for terminal use) and reports the path.
          </p>
          <div className="flex flex-wrap gap-2 mt-3">
            <button
              onClick={recordAndView}
              disabled={loading}
              className={btnOutline}
              title="Stream the current recorder ring into the browser and plot it"
            >
              {loading ? 'Working…' : 'Record & view'}
            </button>
            <button
              onClick={() => fileInputRef.current?.click()}
              disabled={loading}
              className={btnOutline}
              title="Load and decode a .mmpd dump you saved earlier"
            >
              Open .mmpd file
            </button>
            <button
              onClick={() => dumpMutation.mutate()}
              disabled={dumpMutation.isPending}
              className={btnOutline}
              title="Write a .mmpd dump on the machine running Motion Master and report its path"
            >
              {dumpMutation.isPending ? 'Dumping…' : 'Dump to disk'}
            </button>
            <input
              ref={fileInputRef}
              type="file"
              accept=".mmpd,application/octet-stream"
              onChange={onPickFile}
              className="hidden"
            />
          </div>
          {viewError && <p className="text-[11px] text-status-bad mt-2">{viewError}</p>}
          {dumpMutation.isSuccess && (
            <p className="text-[11px] text-status-good mt-2 break-all">
              Wrote <span className="font-mono">{dumpMutation.data.data.path}</span>
            </p>
          )}
          {dumpMutation.isError && (
            <p className="text-[11px] text-status-bad mt-2">{apiError(dumpMutation.error)}</p>
          )}
        </div>

        {file && (
          <div className="space-y-4">
            <div className="text-[11px] text-grey-600">
              <span className="font-medium text-grey-800">{source}</span>
              {' · '}
              {file.header.devices.length} device{file.header.devices.length === 1 ? '' : 's'}
              {' · '}cycle {file.header.cyclePeriodUs} µs
            </div>

            {chart.labels.length > 0 ? (
              <div>
                <CycleStatsBar stats={stats} />
                <MonitoringChart data={chart.data} labels={chart.labels} titles={chart.titles} />
              </div>
            ) : (
              <p className="text-sm text-grey-500">Select one or more objects below to plot.</p>
            )}

            <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
              {byDevice.map((group) => (
                <div key={group.position} className="border border-grey-200 px-3 py-3">
                  <div className="flex items-center gap-2 mb-1.5">
                    <SlavePositionBadge position={group.position} />
                    <span className="text-[11px] font-medium text-grey-800 truncate">
                      {group.name}
                    </span>
                  </div>
                  <div className="grid grid-cols-1 sm:grid-cols-2 gap-x-6">
                    <div>
                      <p className="text-[10px] uppercase tracking-wide text-grey-400 mb-0.5">
                        Outputs
                      </p>
                      {group.outputs.length === 0 ? (
                        <p className="text-[11px] text-grey-300">None</p>
                      ) : (
                        group.outputs.map((item) => (
                          <EntryCheckbox
                            key={item.key}
                            item={item}
                            checked={selected.has(item.key)}
                            onToggle={() => toggle(item.key)}
                          />
                        ))
                      )}
                    </div>
                    <div>
                      <p className="text-[10px] uppercase tracking-wide text-grey-400 mb-0.5">
                        Inputs
                      </p>
                      {group.inputs.length === 0 ? (
                        <p className="text-[11px] text-grey-300">None</p>
                      ) : (
                        group.inputs.map((item) => (
                          <EntryCheckbox
                            key={item.key}
                            item={item}
                            checked={selected.has(item.key)}
                            onToggle={() => toggle(item.key)}
                          />
                        ))
                      )}
                    </div>
                  </div>
                </div>
              ))}
            </div>
          </div>
        )}
      </div>
    </div>
  )
}
