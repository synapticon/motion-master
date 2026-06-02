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
      {},
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
      scales: { x: { time: true } },
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
