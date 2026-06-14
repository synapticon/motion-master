export interface CycleStats {
  mean: number
  median: number
  stdev: number
  min: number
  max: number
}

// Statistics of the cycle time — the Δt (µs) between consecutive samples — over the supplied x
// values. Each sample carries its real cycle timestamp, so these reflect the true cycle period and
// its jitter (e.g. mean ≈ 1000 µs for a 1 ms loop), not the WebSocket flush cadence. stdev is the
// population standard deviation, the headline jitter figure. xs is assumed monotonic and in µs.
export function cycleStats(xs: ArrayLike<number>): CycleStats | null {
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

export function micros(v: number): string {
  return `${Math.round(v).toLocaleString()} µs`
}
