import { useEffect, useState } from 'react'
import { Pause, Play } from 'lucide-react'

// How three fixed coils point a field in any direction, for the Learn pages.
//
// This is the step between "a phase and its return make one electromagnet" and "the drive chooses
// which way the field points". Nothing here moves mechanically. The three axes are bolted in place
// 120 degrees apart; only the currents change, and their sum can be aimed anywhere.
//
// The maths is one line and it is worth stating because the figure is otherwise a magic trick. With
// the three currents set to cos(θ), cos(θ − 120) and cos(θ − 240), the vector sum of the three axis
// contributions comes to 1.5 in the direction θ — exactly. The magnitude never varies as θ turns,
// which is why a smoothly commutated motor produces smooth torque.
//
// The axes are drawn at 0, 120 and 240 degrees. Where they actually sit inside a real machine
// depends on how it is wound; only the 120 degree spacing is common to all of them.

const SIZE = 260
const C = SIZE / 2
const R = 96

const rad = (deg: number) => (deg * Math.PI) / 180
const polar = (r: number, deg: number) => ({ x: C + r * Math.cos(rad(deg)), y: C - r * Math.sin(rad(deg)) })

const PHASES = [
  { label: 'U', axis: 0 },
  { label: 'V', axis: 120 },
  { label: 'W', axis: 240 },
]

function Arrow({ deg, length, className, width = 3 }: { deg: number; length: number; className: string; width?: number }) {
  if (Math.abs(length) < 2) {
    return null
  }
  const sign = length < 0 ? 180 : 0
  const at = deg + sign
  const size = Math.abs(length)
  const tail = polar(0, at)
  const tip = polar(size, at)
  const left = polar(size - 10, at + 6)
  const right = polar(size - 10, at - 6)
  return (
    <g>
      <line x1={tail.x} y1={tail.y} x2={tip.x} y2={tip.y} className={`stroke-current ${className}`} strokeWidth={width} strokeLinecap="round" />
      <polygon points={`${tip.x},${tip.y} ${left.x},${left.y} ${right.x},${right.y}`} className={`fill-current ${className}`} />
    </g>
  )
}

export default function StatorFieldFigure() {
  const [angle, setAngle] = useState(35)
  const [spinning, setSpinning] = useState(false)

  useEffect(() => {
    if (!spinning) {
      return
    }
    let frame = 0
    let previous = performance.now()
    const step = (now: number) => {
      const seconds = (now - previous) / 1000
      previous = now
      setAngle(a => (a + seconds * 60) % 360)
      frame = requestAnimationFrame(step)
    }
    frame = requestAnimationFrame(step)
    return () => cancelAnimationFrame(frame)
  }, [spinning])

  const currents = PHASES.map(phase => Math.cos(rad(angle - phase.axis)))

  // Field lines: parallel chords across the bore, all pointing the way the resultant does.
  const lines = [-0.66, -0.33, 0, 0.33, 0.66].map(offset => {
    const d = offset * R
    const half = Math.sqrt(Math.max(R * R - d * d, 0)) * 0.92
    const n = { x: Math.cos(rad(angle + 90)), y: -Math.sin(rad(angle + 90)) }
    const t = { x: Math.cos(rad(angle)), y: -Math.sin(rad(angle)) }
    const cx = C + n.x * d
    const cy = C + n.y * d
    return {
      x1: cx - t.x * half,
      y1: cy - t.y * half,
      x2: cx + t.x * half,
      y2: cy + t.y * half,
    }
  })

  return (
    <div className="border border-grey-200 max-w-2xl">
      <div className="grid sm:grid-cols-2 divide-y sm:divide-y-0 sm:divide-x divide-grey-200">
        <div className="p-4">
          <svg viewBox={`0 0 ${SIZE} ${SIZE}`} className="w-full max-w-[300px] mx-auto" role="img"
            aria-label={`Three phase currents summing to one field pointing at ${angle.toFixed(0)} degrees`}>
            <defs>
              <marker id="fieldTip" markerWidth="6" markerHeight="6" refX="5" refY="3" orient="auto">
                <path d="M0,0 L6,3 L0,6 Z" className="fill-ocean/50" />
              </marker>
            </defs>

            <circle cx={C} cy={C} r={R} className="fill-grey-50 stroke-grey-200" strokeWidth={1} />

            {/* The field in the bore: parallel lines, all running the way the sum points. */}
            {lines.map((line, i) => (
              <line key={i} {...line} className="stroke-ocean/50" strokeWidth={1.5} markerEnd="url(#fieldTip)" />
            ))}

            {/* The three fixed axes, and each phase's contribution along its own axis. */}
            {PHASES.map((phase, i) => {
              const a = polar(R + 12, phase.axis)
              const b = polar(R + 12, phase.axis + 180)
              return (
                <g key={phase.label}>
                  <line x1={a.x} y1={a.y} x2={b.x} y2={b.y} className="stroke-grey-300" strokeWidth={1} strokeDasharray="3 3" />
                  <text {...polar(R + 24, phase.axis)} textAnchor="middle" dominantBaseline="middle"
                    className="fill-grey-500 text-[11px] font-mono">
                    {phase.label}
                  </text>
                  <Arrow deg={phase.axis} length={currents[i] * 72} className="text-grey-500" width={2} />
                </g>
              )
            })}

            {/* The sum. Its length is 1.5 of a single phase, and never changes as the angle turns. */}
            <Arrow deg={angle} length={78} className="text-syn-red" width={3.5} />
            <circle cx={C} cy={C} r={4} className="fill-grey-800" />
          </svg>
        </div>

        <div className="p-4 space-y-4">
          <div>
            <label className="flex items-baseline justify-between text-xs text-grey-700 mb-1.5">
              <span>Field direction the drive wants</span>
              <span className="font-mono text-grey-900">{angle.toFixed(0)}°</span>
            </label>
            <input
              type="range"
              min={0}
              max={359}
              step={1}
              value={Math.round(angle)}
              onChange={e => {
                setSpinning(false)
                setAngle(Number(e.target.value))
              }}
              className="w-full h-1 appearance-none bg-grey-200 accent-syn-red cursor-pointer"
            />
          </div>

          <div className="space-y-2">
            <p className="text-[11px] text-grey-500 leading-4">Current in each phase, as a share of peak</p>
            {PHASES.map((phase, i) => (
              <div key={phase.label}>
                <div className="flex items-baseline justify-between text-[11px] text-grey-600 mb-1">
                  <span className="font-mono">Phase {phase.label}</span>
                  <span className="font-mono text-grey-900">{(currents[i] * 100).toFixed(0)}%</span>
                </div>
                <div className="relative h-2 bg-grey-100">
                  <div className="absolute inset-y-0 left-1/2 w-px bg-grey-400" />
                  <div
                    className={currents[i] >= 0 ? 'absolute inset-y-0 bg-ocean' : 'absolute inset-y-0 bg-syn-red'}
                    style={{
                      left: currents[i] >= 0 ? '50%' : `${50 - Math.abs(currents[i]) * 50}%`,
                      width: `${Math.abs(currents[i]) * 50}%`,
                    }}
                  />
                </div>
              </div>
            ))}
          </div>

          <button
            type="button"
            onClick={() => setSpinning(s => !s)}
            className="h-[38px] inline-flex items-center gap-1.5 px-3 text-xs border border-syn-red text-syn-red hover:bg-syn-red hover:text-white transition-colors cursor-pointer"
          >
            {spinning ? <Pause className="h-3.5 w-3.5" aria-hidden /> : <Play className="h-3.5 w-3.5" aria-hidden />}
            {spinning ? 'Stop' : 'Turn the field'}
          </button>
        </div>
      </div>

      <div className="border-t border-grey-200 p-4 text-[11px] text-grey-500 leading-4 space-y-1.5">
        <p>
          <span className="text-grey-900">Nothing here moves.</span> The three axes are fixed in the
          stator, 120 degrees apart. Only the currents change, and the grey arrows are each
          phase&apos;s pull along its own axis — forwards when its current is positive, backwards
          when it is negative.
        </p>
        <p>
          The red arrow is the three added together, and the blue lines are the field it makes across
          the bore. Turn the field and watch the red arrow sweep smoothly through directions no
          single coil points in. That is the whole trick: three fixed coils, one field, aimed
          anywhere.
        </p>
        <p>
          Each percentage is the current in that phase at that instant, measured against the peak the
          drive is using. A negative one means the current runs the other way through that winding.
          Add the three up at any angle and they come to zero, which they have to: there are three
          wires and no fourth, so whatever goes in through one phase comes back through the other
          two.
        </p>
        <p>
          Turn the field and each bar traces a sine wave, the three of them a third of a cycle apart.
          That is what <strong>three-phase</strong> means.
        </p>
        <p>
          The red arrow&apos;s length never changes as it turns, which is not a coincidence either —
          three currents shaped like <span className="font-mono">cos(θ)</span>,{' '}
          <span className="font-mono">cos(θ−120)</span> and <span className="font-mono">cos(θ−240)</span>{' '}
          always sum to the same magnitude. A motor driven this way makes steady torque rather than a
          lumpy one.
        </p>
      </div>
    </div>
  )
}
