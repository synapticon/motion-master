import { useEffect, useRef } from 'react'
import uPlot from 'uplot'
import 'uplot/dist/uPlot.min.css'

// Distinct series colours (Synapticon-ish palette), cycled for parameters beyond the list.
const COLORS = [
  '#e2001a', // syn-red
  '#0a6c9c', // ocean
  '#2e7d32',
  '#f9a825',
  '#6a1b9a',
  '#00838f',
  '#c2185b',
  '#5d4037',
]

const HEIGHT = 240

// x values are microseconds elapsed since the first sample. The cursor/hover readout always shows
// exact microseconds — the point the user is reading off the trace — since the default cycle is
// 1 ms and can be shorter, so µs is the resolution that matters. Grouped for readability.
function formatMicros(us: number): string {
  return `${us.toLocaleString(undefined, { maximumFractionDigits: 3 })} µs`
}

// Axis tick labels, by contrast, use one shared larger unit (chosen from the biggest tick) so a
// wide window's labels stay short and don't overlap — e.g. "250 ms", "1 s" — while the trace
// itself and the hover readout keep full µs resolution.
function formatTicks(splits: number[]): string[] {
  const max = splits.reduce((m, v) => Math.max(m, Math.abs(v)), 0)
  const [div, unit] = max >= 1_000_000 ? [1_000_000, 's'] : max >= 1_000 ? [1_000, 'ms'] : [1, 'µs']
  return splits.map((v) => {
    const n = v / div
    return `${Number.isInteger(n) ? n.toString() : n.toFixed(3)} ${unit}`
  })
}

/// Thin uPlot wrapper for a live time-series. Re-creates the plot when the series set changes
/// (pass a stable, memoised @p labels), and pushes new data via setData otherwise — so streaming
/// updates never tear down the canvas.
export default function MonitoringChart({
  data,
  labels,
}: {
  data: uPlot.AlignedData
  labels: string[]
}) {
  const containerRef = useRef<HTMLDivElement>(null)
  const plotRef = useRef<uPlot | null>(null)
  const dataRef = useRef<uPlot.AlignedData>(data)
  dataRef.current = data

  // Create (and re-create when the series change). Reads the latest data via ref.
  useEffect(() => {
    const el = containerRef.current
    if (!el) return

    const series: uPlot.Series[] = [
      // x is elapsed microseconds, not a timestamp — format the cursor/legend readout in µs.
      { label: 'Elapsed', value: (_u, v) => (v == null ? '—' : formatMicros(v)) },
      ...labels.map((label, i) => ({
        label,
        stroke: COLORS[i % COLORS.length],
        width: 1,
        spanGaps: false, // null samples (device not exchanging) leave gaps
      })),
    ]
    const opts: uPlot.Options = {
      width: el.clientWidth || 600,
      height: HEIGHT,
      series,
      // time:false — x is relative elapsed microseconds, so uPlot ticks in linear µs (down to the
      // cycle resolution) instead of clamping to whole-second wall-clock. Tick labels adapt their
      // unit so they don't overlap; the hover readout (series value above) stays in exact µs.
      scales: { x: { time: false } },
      axes: [{ values: (_u, splits) => formatTicks(splits) }, {}],
      legend: { live: true },
      cursor: { drag: { x: true, y: false } },
    }
    const plot = new uPlot(opts, dataRef.current, el)
    plotRef.current = plot

    const ro = new ResizeObserver(() => {
      plot.setSize({ width: el.clientWidth || 600, height: HEIGHT })
    })
    ro.observe(el)

    return () => {
      ro.disconnect()
      plot.destroy()
      plotRef.current = null
    }
  }, [labels])

  // Stream new data into the existing plot.
  useEffect(() => {
    plotRef.current?.setData(data)
  }, [data])

  return <div ref={containerRef} className="w-full" />
}
