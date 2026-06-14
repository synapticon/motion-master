import { type CycleStats, micros } from '../utils/cycleStats'

// The cycle-time readout shown above a plot: mean / median / σ / min / max of the Δt between
// consecutive samples. Shared by the live monitoring chart and the recorder RECORD & VIEW chart so
// both report jitter the same way.
export default function CycleStatsBar({ stats }: { stats: CycleStats | null }) {
  if (!stats) return null
  return (
    <div
      className="mb-2 text-center text-xs text-grey-500 cursor-help"
      title="Cycle time — Δt between consecutive samples"
    >
      <span className="uppercase tracking-wide text-grey-400">Cycle time</span>{' '}
      mean <span className="font-mono text-grey-700">{micros(stats.mean)}</span> · median{' '}
      <span className="font-mono text-grey-700">{micros(stats.median)}</span> · σ{' '}
      <span className="font-mono text-grey-700">{micros(stats.stdev)}</span> · min{' '}
      <span className="font-mono text-grey-700">{micros(stats.min)}</span> · max{' '}
      <span className="font-mono text-grey-700">{micros(stats.max)}</span>
    </div>
  )
}
