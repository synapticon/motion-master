import { useEffect, useRef } from 'react'
import uPlot from 'uplot'
import 'uplot/dist/uPlot.min.css'

// Distinct series colours (Synapticon-ish palette), cycled for parameters beyond the list.
const COLORS = [
  '#e0004d', // syn-red
  '#00849b', // ocean
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

/** How the x values read: the legend/cursor label, the hover readout, and the tick labels. */
export interface XAxis {
  label: string
  format: (v: number) => string
  ticks: (splits: number[]) => string[]
}

/** x is microseconds elapsed since the first sample — the live monitoring and recorder case. */
export const ELAPSED_MICROSECONDS: XAxis = {
  label: 'Elapsed',
  format: formatMicros,
  ticks: formatTicks,
}

/** x is a position in a sequence, with no time in it — a recording read back off a device, say. */
export const SAMPLE_INDEX: XAxis = {
  label: 'Sample',
  format: (v) => v.toLocaleString(),
  ticks: (splits) => splits.map((v) => v.toLocaleString()),
}

/**
 * Draws the reference geometry, in a uPlot `draw` hook reading the latest annotations from a ref.
 *
 * Everything is clipped to the plot area and drawn under the same save/restore, so a band that
 * extends past the current zoom paints to the edge rather than over the axes. Labels are skipped
 * when they would land on top of the previous one - a dozen overlapping strings is worse than none,
 * and a stop trace legitimately has several markers within a few milliseconds of each other.
 */
function drawAnnotations(ref: { current: ChartAnnotations | undefined }) {
  return (u: uPlot) => {
    const a = ref.current
    if (!a) return
    const { ctx } = u
    const { left, top, width, height } = u.bbox
    ctx.save()
    ctx.beginPath()
    ctx.rect(left, top, width, height)
    ctx.clip()

    const xPos = (v: number) => u.valToPos(v, 'x', true)
    const yPos = (v: number) => u.valToPos(v, 'y', true)
    let lastLabelX = -Infinity
    const label = (text: string, x: number, y: number, color: string) => {
      if (x - lastLabelX < 44) return
      lastLabelX = x
      ctx.fillStyle = color
      ctx.font = '10px ui-sans-serif, system-ui, sans-serif'
      ctx.fillText(text, x + 3, y)
    }

    for (const b of a.hBands ?? []) {
      const y0 = yPos(b.to)
      const y1 = yPos(b.from)
      ctx.fillStyle = b.color ?? 'rgba(16,185,129,0.12)'
      ctx.fillRect(left, Math.min(y0, y1), width, Math.abs(y1 - y0))
    }
    for (const b of a.vBands ?? []) {
      const x0 = xPos(b.from)
      const x1 = xPos(b.to)
      ctx.fillStyle = b.color ?? 'rgba(245,158,11,0.12)'
      ctx.fillRect(Math.min(x0, x1), top, Math.abs(x1 - x0), height)
    }
    for (const l of a.vLines ?? []) {
      const x = xPos(l.x)
      ctx.strokeStyle = l.color ?? '#6b7280'
      ctx.lineWidth = 1
      ctx.setLineDash(l.dash ?? [])
      ctx.beginPath()
      ctx.moveTo(x, top)
      ctx.lineTo(x, top + height)
      ctx.stroke()
      ctx.setLineDash([])
      if (l.label) label(l.label, x, top + 11, l.color ?? '#6b7280')
    }
    for (const p of a.points ?? []) {
      const x = xPos(p.x)
      const y = yPos(p.y)
      ctx.strokeStyle = p.color ?? '#111827'
      ctx.lineWidth = 2
      ctx.beginPath()
      ctx.arc(x, y, 4, 0, Math.PI * 2)
      ctx.stroke()
      if (p.label) label(p.label, x + 4, y - 6, p.color ?? '#111827')
    }
    ctx.restore()
  }
}

/** x is microseconds from an event, so negative values are context from before it. */
export const SINCE_TRIGGER: XAxis = {
  label: 'From trigger',
  format: formatMicros,
  ticks: formatTicks,
}

/** Reference geometry drawn over the series: what the data should be measured against. */
export interface ChartAnnotations {
  /** A vertical line at an x value - an instant. */
  vLines?: { x: number; label?: string; color?: string; dash?: number[] }[]
  /** A shaded x range - an interval. */
  vBands?: { from: number; to: number; color?: string; label?: string }[]
  /** A shaded y range - a limit band such as a standstill window. */
  hBands?: { from: number; to: number; color?: string; label?: string }[]
  /** A marked (x, y) - an event at a value. */
  points?: { x: number; y: number; label?: string; color?: string }[]
}

/// Thin uPlot wrapper for a live time-series. Re-creates the plot when the series set changes and
/// pushes new data via setData otherwise — so streaming updates never tear down the canvas.
///
/// The rebuild is keyed on the *content* of @p labels / @p titles, not their array identity, so a
/// caller may pass a freshly-built array every render (the natural thing when projecting streamed
/// data). Keying on reference instead would rebuild the canvas on every batch (~60×/s), collapsing
/// the chart to zero height and yanking the page scroll back to the top.
export default function MonitoringChart({
  data,
  labels,
  titles = [],
  hidden = [],
  colors = [],
  dashes = [],
  annotations,
  xAxis = ELAPSED_MICROSECONDS,
}: {
  data: uPlot.AlignedData
  labels: string[]
  /** Optional hover tooltip per series (e.g. the parameter name), aligned with `labels`. */
  titles?: string[]
  /** Labels whose series start hidden. A default only — the user can toggle them on via the
   *  legend, and those toggles survive data updates (applied once, at plot-create time). */
  hidden?: string[]
  /** Optional per-series stroke colour, aligned with `labels`; a blank/undefined entry falls back
   *  to the built-in palette. Changing a colour rebuilds the plot (keyed on content). */
  colors?: (string | undefined)[]
  /** Optional per-series dash pattern, aligned with `labels`. Joins the rebuild key, so it is a
   *  plain array of numbers rather than anything that would only compare by reference. */
  dashes?: (number[] | undefined)[]
  /** Reference geometry drawn over the series.
   *
   *  Passed as DATA rather than as a uPlot plugin on purpose. The rebuild below is keyed on the
   *  *content* of the props, and a plugin is a function - it cannot be serialised into that key, so
   *  handing one in would either rebuild the canvas on every render (which collapses it to zero
   *  height and yanks the page scroll) or never pick up a change. A built-in draw hook reading a ref
   *  avoids both. */
  annotations?: ChartAnnotations
  /** What the x values mean. Defaults to elapsed microseconds; pass SAMPLE_INDEX for data whose
   *  x is a position rather than a time, so the ticks do not read as µs. */
  xAxis?: XAxis
}) {
  const containerRef = useRef<HTMLDivElement>(null)
  const plotRef = useRef<uPlot | null>(null)
  const dataRef = useRef<uPlot.AlignedData>(data)
  dataRef.current = data
  // Latest label/title arrays, read inside the effects that are keyed on their content below.
  const labelsRef = useRef(labels)
  labelsRef.current = labels
  const titlesRef = useRef(titles)
  titlesRef.current = titles
  const hiddenRef = useRef(hidden)
  hiddenRef.current = hidden
  const colorsRef = useRef(colors)
  colorsRef.current = colors
  const xAxisRef = useRef(xAxis)
  xAxisRef.current = xAxis
  const dashesRef = useRef(dashes)
  dashesRef.current = dashes
  const annotationsRef = useRef(annotations)
  annotationsRef.current = annotations
  // Content keys — a stable string identity for a given set of labels/titles/colors. These, not the
  // array references, drive the effects, so a caller passing a fresh array each render is harmless.
  const labelsKey = JSON.stringify(labels)
  const titlesKey = JSON.stringify(titles)
  const colorsKey = JSON.stringify(colors)
  const dashesKey = JSON.stringify(dashes)
  const annotationsKey = JSON.stringify(annotations)

  // Create (and re-create when the series set actually changes). Reads the latest data/labels via
  // refs, so it fires only when labelsKey changes — never on a per-batch data update.
  useEffect(() => {
    const el = containerRef.current
    if (!el) return

    const series: uPlot.Series[] = [
      // x is never a wall-clock timestamp here — it is elapsed µs or a sample position — so the
      // cursor/legend readout is formatted by the caller's axis rather than by uPlot's date logic.
      { label: xAxisRef.current.label, value: (_u, v) => (v == null ? '—' : xAxisRef.current.format(v)) },
      ...labelsRef.current.map((label, i) => ({
        label,
        stroke: colorsRef.current[i] || COLORS[i % COLORS.length],
        width: 1,
        dash: dashesRef.current[i],
        spanGaps: false, // null samples (device not exchanging) leave gaps
        show: !hiddenRef.current.includes(label), // initial visibility; legend can toggle it back on
      })),
    ]
    const opts: uPlot.Options = {
      width: el.clientWidth || 600,
      height: HEIGHT,
      series,
      // time:false — x is a relative quantity (elapsed µs, or a sample position), so uPlot ticks it
      // linearly instead of clamping to whole-second wall-clock. Tick labels adapt so they don't
      // overlap; the hover readout (series value above) keeps full resolution.
      scales: { x: { time: false } },
      axes: [{ values: (_u, splits) => xAxisRef.current.ticks(splits) }, {}],
      legend: { live: true },
      cursor: { drag: { x: true, y: false } },
      plugins: [{ hooks: { draw: [drawAnnotations(annotationsRef)] } }],
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
    // xAxis.label joins the rebuild key so a page that switches what x means (and only then) gets a
    // plot whose legend and ticks agree with its data.
  }, [labelsKey, colorsKey, dashesKey, xAxis.label])

  // Annotations live in a ref, so a change to them alone needs an explicit repaint. Keyed on their
  // content for the same reason as everything else here: the caller builds them fresh each render.
  useEffect(() => {
    plotRef.current?.redraw()
  }, [annotationsKey])

  // Stream new data into the existing plot.
  useEffect(() => {
    plotRef.current?.setData(data)
  }, [data])

  // Set each legend row's hover title to the parameter name. uPlot renders the legend as an HTML
  // table; the first `.u-series` row is the x-axis ("Elapsed"), so series rows start at index 1.
  // Re-applied whenever the plot is rebuilt (labelsKey / colorsKey) or the names resolve (titlesKey).
  useEffect(() => {
    const rows = containerRef.current?.querySelectorAll<HTMLElement>('.u-legend tr.u-series')
    if (!rows) return
    titlesRef.current.forEach((title, i) => {
      const row = rows[i + 1]
      if (row) row.title = title
    })
  }, [titlesKey, labelsKey, colorsKey])

  return <div ref={containerRef} className="w-full" />
}
