import { useCallback, useEffect, useRef, useState } from 'react'
import { Crosshair, Pause, Play, RotateCcw } from 'lucide-react'
import PolePairSelector from './PolePairSelector'

// The interactive model behind the Learn → Commutation Offset page.
//
// Two coupled dials. The left one is the machine as it physically is: a rotor whose magnets sit at
// some mechanical angle, and an encoder disc glued to the same shaft with its zero mark wherever
// assembly happened to leave it. The right one is the electrical angle the current controller
// actually works in, where the rotor's magnet axis and the drive's belief about it are two separate
// arrows and the angle between them is the whole story.
//
// The maths is small and worth stating, because every readout falls out of it. With `shaft` the
// true mechanical angle, `mount` the mechanical angle at which the encoder reads zero, and `p` pole
// pairs:
//
//   encoderReading   = shaft - mount
//   trueElectrical   = shaft * p
//   driveElectrical  = encoderReading * p + storedOffset
//   error            = driveElectrical - trueElectrical
//
// Substituting gives error = storedOffset - mount * p, so the offset that makes the drive right is
// `mount * p` and it does not depend on where the shaft is. That is why the measurement is a
// one-off per assembly rather than something the drive keeps re-doing.
//
// Torque follows from projecting the applied current onto the axis that actually produces torque:
// cos(error) of it makes torque, sin(error) of it makes only heat.

const norm360 = (deg: number) => ((deg % 360) + 360) % 360
const signed180 = (deg: number) => norm360(deg + 180) - 180
const rad = (deg: number) => (deg * Math.PI) / 180

const SIZE = 280
const C = SIZE / 2

// Screen point for a maths-convention angle: 0 at the right, increasing counter-clockwise. SVG's y
// axis points down, hence the subtraction.
function polar(r: number, deg: number) {
  return { x: C + r * Math.cos(rad(deg)), y: C - r * Math.sin(rad(deg)) }
}

// An annulus sector from `fromDeg` counter-clockwise by `spanDeg`. Used for the magnet poles and
// for every shaded angle band, so they all share one path builder.
function sector(rInner: number, rOuter: number, fromDeg: number, spanDeg: number) {
  const span = Math.min(Math.max(spanDeg, 0.001), 359.999)
  const large = span > 180 ? 1 : 0
  const a = polar(rOuter, fromDeg)
  const b = polar(rOuter, fromDeg + span)
  const c = polar(rInner, fromDeg + span)
  const d = polar(rInner, fromDeg)
  return [
    `M ${a.x.toFixed(2)} ${a.y.toFixed(2)}`,
    `A ${rOuter} ${rOuter} 0 ${large} 0 ${b.x.toFixed(2)} ${b.y.toFixed(2)}`,
    `L ${c.x.toFixed(2)} ${c.y.toFixed(2)}`,
    `A ${rInner} ${rInner} 0 ${large} 1 ${d.x.toFixed(2)} ${d.y.toFixed(2)}`,
    'Z',
  ].join(' ')
}

// A line from the centre out to `r`, the shape every pointer and axis marker on both dials uses.
function Ray({
  deg,
  from = 0,
  to,
  className,
  width = 2,
  dashed = false,
}: {
  deg: number
  from?: number
  to: number
  className: string
  width?: number
  dashed?: boolean
}) {
  const a = polar(from, deg)
  const b = polar(to, deg)
  return (
    <line
      x1={a.x}
      y1={a.y}
      x2={b.x}
      y2={b.y}
      className={className}
      strokeWidth={width}
      strokeLinecap="round"
      strokeDasharray={dashed ? '4 3' : undefined}
    />
  )
}

// An arrowhead at the tip of a ray, so the two vectors on the electrical dial read as vectors.
function Arrowhead({ deg, r, className }: { deg: number; r: number; className: string }) {
  const tip = polar(r, deg)
  const left = polar(r - 11, deg + 5)
  const right = polar(r - 11, deg - 5)
  return (
    <polygon
      points={`${tip.x},${tip.y} ${left.x},${left.y} ${right.x},${right.y}`}
      className={className}
    />
  )
}

type DragTarget = 'rotor' | 'encoder'

const sliderClass =
  'w-full h-1 appearance-none bg-grey-200 accent-syn-red cursor-pointer disabled:cursor-not-allowed'

// One labelled row of the readout table.
function Readout({
  label,
  value,
  hint,
  emphasis = false,
}: {
  label: string
  value: string
  hint?: string
  emphasis?: boolean
}) {
  return (
    <div className="flex items-baseline justify-between gap-4 py-1.5 border-b border-grey-100 last:border-0">
      <span className="text-xs text-grey-600 min-w-0">
        {label}
        {hint && <span className="block text-[11px] text-grey-500 leading-4">{hint}</span>}
      </span>
      <span
        className={`font-mono text-xs whitespace-nowrap ${emphasis ? 'text-syn-red font-semibold' : 'text-grey-900'}`}
      >
        {value}
      </span>
    </div>
  )
}

// One legend entry: a short line in the colour it explains, then the label. Both dials carry one,
// because four unlabelled strokes on a circle is a puzzle rather than a diagram.
function Key({ className, label, dashed = false }: { className: string; label: string; dashed?: boolean }) {
  return (
    <span className="inline-flex items-center gap-1.5 text-[11px] text-grey-600">
      <svg width="18" height="8" aria-hidden className="shrink-0">
        <line
          x1="1"
          y1="4"
          x2="17"
          y2="4"
          className={className}
          strokeWidth={2.5}
          strokeLinecap="round"
          strokeDasharray={dashed ? '4 3' : undefined}
        />
      </svg>
      {label}
    </span>
  )
}

export default function CommutationOffsetLab({
  polePairs,
  onPolePairsChange,
}: {
  polePairs: number
  onPolePairsChange: (polePairs: number) => void
}) {
  const [shaft, setShaft] = useState(35)
  const [mount, setMount] = useState(110)
  const [storedOffset, setStoredOffset] = useState(0)
  const [spinning, setSpinning] = useState(false)

  const drag = useRef<{ target: DragTarget; grab: number } | null>(null)

  const encoderReading = norm360(shaft - mount)
  const trueElectrical = norm360(shaft * polePairs)
  const driveElectrical = norm360(encoderReading * polePairs + storedOffset)
  const correctOffset = norm360(mount * polePairs)
  const error = signed180(driveElectrical - trueElectrical)
  const torque = Math.cos(rad(error))
  const heatOnly = Math.abs(Math.sin(rad(error)))
  const aligned = Math.abs(error) < 0.5

  // The rotor turns on its own so the two dials can be watched together: the electrical angle
  // advances `polePairs` times per mechanical turn, and a wrong offset holds its lag the whole way
  // round rather than washing out.
  useEffect(() => {
    if (!spinning) {
      return
    }
    let frame = 0
    let previous = performance.now()
    const step = (now: number) => {
      const seconds = (now - previous) / 1000
      previous = now
      setShaft(s => norm360(s + seconds * 36))
      frame = requestAnimationFrame(step)
    }
    frame = requestAnimationFrame(step)
    return () => cancelAnimationFrame(frame)
  }, [spinning])

  const pointerAngle = useCallback((event: React.PointerEvent<SVGSVGElement>) => {
    const rect = event.currentTarget.getBoundingClientRect()
    const x = ((event.clientX - rect.left) / rect.width) * SIZE - C
    const y = C - ((event.clientY - rect.top) / rect.height) * SIZE
    return { deg: norm360((Math.atan2(y, x) * 180) / Math.PI), radius: Math.hypot(x, y) }
  }, [])

  const onPointerDown = (event: React.PointerEvent<SVGSVGElement>) => {
    const { deg, radius } = pointerAngle(event)
    if (radius <= 74) {
      drag.current = { target: 'rotor', grab: norm360(shaft - deg) }
    } else if (radius <= 116) {
      drag.current = { target: 'encoder', grab: norm360(encoderReading - deg) }
    } else {
      return
    }
    setSpinning(false)
    event.currentTarget.setPointerCapture(event.pointerId)
  }

  const onPointerMove = (event: React.PointerEvent<SVGSVGElement>) => {
    if (!drag.current) {
      return
    }
    const { deg } = pointerAngle(event)
    const angle = norm360(deg + drag.current.grab)
    if (drag.current.target === 'rotor') {
      setShaft(angle)
    } else {
      // Turning the disc against a stationary shaft is exactly what re-mounting the encoder does,
      // so the grabbed zero mark follows the pointer and the mounting offset absorbs the change.
      setMount(norm360(shaft - angle))
    }
  }

  const endDrag = (event: React.PointerEvent<SVGSVGElement>) => {
    if (drag.current) {
      event.currentTarget.releasePointerCapture(event.pointerId)
      drag.current = null
    }
  }

  const reset = () => {
    setSpinning(false)
    setShaft(35)
    setMount(110)
    setStoredOffset(0)
    onPolePairsChange(1)
  }

  const poles = Array.from({ length: polePairs * 2 }, (_, i) => i)
  const poleSpan = 180 / polePairs

  return (
    <div className="border border-grey-200">
      <div className="grid lg:grid-cols-2 divide-y lg:divide-y-0 lg:divide-x divide-grey-200">
        {/* ── The machine, as it physically is ─────────────────────────────── */}
        <div className="p-4">
          <h3 className="text-xs font-medium text-grey-900 mb-1">The machine</h3>
          <p className="text-[11px] text-grey-600 leading-4 mb-3">
            Drag the rotor to turn the shaft. Drag the outer ring to re-mount the encoder disc.
          </p>
          <svg
            viewBox={`0 0 ${SIZE} ${SIZE}`}
            className="w-full max-w-[320px] mx-auto touch-none select-none"
            role="img"
            aria-label={`Rotor at ${shaft.toFixed(0)} degrees mechanical, encoder reading ${encoderReading.toFixed(0)} degrees`}
            onPointerDown={onPointerDown}
            onPointerMove={onPointerMove}
            onPointerUp={endDrag}
            onPointerCancel={endDrag}
          >
            {/* Encoder disc: the ring that turns with the shaft and carries the zero mark. */}
            <circle cx={C} cy={C} r={104} className="fill-grey-50 stroke-grey-300" strokeWidth={1} />
            <circle cx={C} cy={C} r={84} className="fill-white stroke-grey-300" strokeWidth={1} />
            {Array.from({ length: 36 }, (_, i) => {
              const a = polar(94, encoderReading + i * 10)
              const b = polar(i % 9 === 0 ? 84 : 89, encoderReading + i * 10)
              return (
                <line
                  key={i}
                  x1={a.x}
                  y1={a.y}
                  x2={b.x}
                  y2={b.y}
                  className="stroke-grey-300"
                  strokeWidth={1}
                />
              )
            })}

            {/* The mounting offset: the arc between the encoder's zero mark and the magnet axis.
                This band is the quantity the whole procedure exists to measure. */}
            <path
              d={sector(70, 82, encoderReading, norm360(mount))}
              className="fill-safety-yellow/35 stroke-safety-yellow"
              strokeWidth={0.75}
            />

            {/* Rotor poles, alternating north and south, turning with the shaft. */}
            {poles.map(i => (
              <path
                key={i}
                d={sector(18, 68, shaft - poleSpan / 2 + i * poleSpan, poleSpan)}
                className={i % 2 === 0 ? 'fill-syn-red/85' : 'fill-ocean/85'}
                stroke="white"
                strokeWidth={1}
              />
            ))}
            <circle cx={C} cy={C} r={18} className="fill-grey-800" />
            <circle cx={C} cy={C} r={5} className="fill-grey-300" />

            {/* Magnet axis — the direction the drive has to know and cannot sense directly. */}
            <Ray deg={shaft} from={18} to={68} className="stroke-grey-900" width={2.5} />
            <text
              {...polar(76, shaft)}
              className="fill-grey-900 text-[10px] font-mono"
              textAnchor="middle"
              dominantBaseline="middle"
            >
              N
            </text>

            {/* Encoder zero mark, engraved on the disc and therefore turning with it. */}
            <Ray deg={encoderReading} from={84} to={104} className="stroke-grey-900" width={3} />
            <text
              {...polar(76, encoderReading)}
              className="fill-grey-900 text-[10px] font-mono"
              textAnchor="middle"
              dominantBaseline="middle"
            >
              0
            </text>

            {/* Read head: fixed in space, so the reading is the angle from the mark to here. */}
            <Ray deg={0} from={104} to={126} className="stroke-grey-400" width={1} dashed />
            <polygon
              points={`${polar(106, 0).x},${polar(106, 0).y} ${polar(118, 4).x},${polar(118, 4).y} ${polar(118, -4).x},${polar(118, -4).y}`}
              className="fill-grey-700"
            />
            <text
              x={C}
              y={SIZE - 4}
              className="fill-grey-500 text-[9px]"
              textAnchor="middle"
            >
              read head fixed to the housing · zero mark turns with the shaft
            </text>
          </svg>
          <div className="flex flex-wrap gap-x-4 gap-y-1 mt-2 justify-center">
            <Key className="stroke-grey-900" label="N — magnet axis" />
            <Key className="stroke-grey-900" label="0 — encoder zero mark" />
            <Key className="stroke-safety-yellow" label="mounting offset" />
          </div>
        </div>

        {/* ── The electrical angle the controller works in ──────────────────── */}
        <div className="p-4">
          <h3 className="text-xs font-medium text-grey-900 mb-1">What the controller sees</h3>
          <p className="text-[11px] text-grey-600 leading-4 mb-3">
            The magnet axis, the drive&apos;s belief about it, and the current it applies.
          </p>
          <svg
            viewBox={`0 0 ${SIZE} ${SIZE}`}
            className="w-full max-w-[320px] mx-auto select-none"
            role="img"
            aria-label={`Rotor electrical angle ${trueElectrical.toFixed(0)} degrees, drive believes ${driveElectrical.toFixed(0)} degrees, error ${error.toFixed(0)} degrees`}
          >
            <circle cx={C} cy={C} r={104} className="fill-grey-50 stroke-grey-200" strokeWidth={1} />

            {/* The error wedge: everything that goes wrong lives in this angle. */}
            {!aligned && (
              <path
                d={
                  error >= 0
                    ? sector(4, 100, trueElectrical, error)
                    : sector(4, 100, driveElectrical, -error)
                }
                className="fill-status-warn/25"
              />
            )}

            {Array.from({ length: 12 }, (_, i) => {
              const a = polar(104, i * 30)
              const b = polar(97, i * 30)
              return (
                <line
                  key={i}
                  x1={a.x}
                  y1={a.y}
                  x2={b.x}
                  y2={b.y}
                  className="stroke-grey-300"
                  strokeWidth={1}
                />
              )
            })}

            {/* Where the current would go if the offset were right — the target, drawn faintly. */}
            <Ray deg={trueElectrical + 90} to={92} className="stroke-ocean/30" width={2} dashed />

            {/* The magnet axis in electrical terms. */}
            <Ray deg={trueElectrical} to={88} className="stroke-grey-900" width={2.5} />
            <Arrowhead deg={trueElectrical} r={96} className="fill-grey-900" />

            {/* What the drive thinks the magnet axis is. */}
            <Ray deg={driveElectrical} to={88} className="stroke-grey-500" width={2} dashed />
            <Arrowhead deg={driveElectrical} r={96} className="fill-grey-500" />

            {/* The current the drive actually applies: a quarter turn ahead of its belief. */}
            <Ray deg={driveElectrical + 90} to={80} className="stroke-syn-red" width={3} />
            <Arrowhead deg={driveElectrical + 90} r={92} className="fill-syn-red" />

            <circle cx={C} cy={C} r={4} className="fill-grey-800" />
          </svg>
          <div className="flex flex-wrap gap-x-4 gap-y-1 mt-2 justify-center">
            <Key className="stroke-grey-900" label="magnets, true" />
            <Key className="stroke-grey-500" label="magnets, as the drive believes" dashed />
            <Key className="stroke-syn-red" label="current applied" />
            <Key className="stroke-ocean/40" label="where it should point" dashed />
          </div>

          <div className="mt-3 space-y-2">
            <div>
              <div className="flex items-baseline justify-between text-[11px] text-grey-600 mb-1">
                <span>Torque from the same current</span>
                <span className="font-mono text-grey-900">{(torque * 100).toFixed(0)}%</span>
              </div>
              <div className="relative h-2 bg-grey-100">
                <div className="absolute inset-y-0 left-1/2 w-px bg-grey-400" />
                <div
                  className={`absolute inset-y-0 ${torque >= 0 ? 'bg-status-good' : 'bg-status-bad'}`}
                  style={{
                    left: torque >= 0 ? '50%' : `${50 - Math.abs(torque) * 50}%`,
                    width: `${Math.abs(torque) * 50}%`,
                  }}
                />
              </div>
            </div>
            <div>
              <div className="flex items-baseline justify-between text-[11px] text-grey-600 mb-1">
                <span>Current making no torque</span>
                <span className="font-mono text-grey-900">{(heatOnly * 100).toFixed(0)}%</span>
              </div>
              <div className="h-2 bg-grey-100">
                <div className="h-full bg-status-warn" style={{ width: `${heatOnly * 100}%` }} />
              </div>
            </div>
          </div>
        </div>
      </div>

      {/* ── Controls ───────────────────────────────────────────────────────── */}
      <div className="border-t border-grey-200 grid md:grid-cols-2 divide-y md:divide-y-0 md:divide-x divide-grey-200">
        <div className="p-4 space-y-4">
          <div>
            <label className="flex items-baseline justify-between text-xs text-grey-700 mb-1.5">
              <span>Shaft angle</span>
              <span className="font-mono text-grey-900">{shaft.toFixed(0)}°</span>
            </label>
            <input
              type="range"
              min={0}
              max={359}
              step={1}
              value={Math.round(shaft)}
              onChange={e => {
                setSpinning(false)
                setShaft(Number(e.target.value))
              }}
              className={sliderClass}
            />
          </div>

          <div>
            <label className="flex items-baseline justify-between text-xs text-grey-700 mb-1.5">
              <span>Encoder mounting</span>
              <span className="font-mono text-grey-900">{mount.toFixed(0)}° mech</span>
            </label>
            <input
              type="range"
              min={0}
              max={359}
              step={1}
              value={Math.round(mount)}
              onChange={e => setMount(Number(e.target.value))}
              className={sliderClass}
            />
            <p className="text-[11px] text-grey-500 leading-4 mt-1">
              Set once by assembly. Nothing on the drive can read it.
            </p>
          </div>

          <PolePairSelector
            value={polePairs}
            onChange={onPolePairsChange}
            hint="The electrical angle turns this many times per turn of the shaft."
          />
        </div>

        <div className="p-4 space-y-4">
          <div>
            <label className="flex items-baseline justify-between text-xs text-grey-700 mb-1.5">
              <span>Stored commutation offset</span>
              <span className="font-mono text-grey-900">{storedOffset.toFixed(0)}° elec</span>
            </label>
            <input
              type="range"
              min={0}
              max={359}
              step={1}
              value={Math.round(storedOffset)}
              onChange={e => setStoredOffset(Number(e.target.value))}
              className={sliderClass}
            />
            <p className="text-[11px] text-grey-500 leading-4 mt-1">
              The one number the drive keeps. Correct here is{' '}
              <span className="font-mono">{correctOffset.toFixed(0)}°</span>.
            </p>
          </div>

          <div className="flex flex-wrap items-center gap-2">
            <button
              type="button"
              onClick={() => setStoredOffset(correctOffset)}
              className="h-[38px] inline-flex items-center gap-1.5 px-3 text-xs bg-syn-red text-white hover:bg-ocean transition-colors cursor-pointer"
            >
              <Crosshair className="h-3.5 w-3.5" aria-hidden />
              Measure the offset
            </button>
            <button
              type="button"
              onClick={() => setSpinning(s => !s)}
              className="h-[38px] inline-flex items-center gap-1.5 px-3 text-xs border border-syn-red text-syn-red hover:bg-syn-red hover:text-white transition-colors cursor-pointer"
            >
              {spinning ? (
                <Pause className="h-3.5 w-3.5" aria-hidden />
              ) : (
                <Play className="h-3.5 w-3.5" aria-hidden />
              )}
              {spinning ? 'Stop' : 'Turn'}
            </button>
            <button
              type="button"
              onClick={reset}
              className="h-[38px] inline-flex items-center gap-1.5 px-3 text-xs border border-grey-300 text-grey-700 hover:border-grey-700 transition-colors cursor-pointer"
            >
              <RotateCcw className="h-3.5 w-3.5" aria-hidden />
              Reset
            </button>
          </div>

          <div>
            <Readout label="Encoder reading" value={`${encoderReading.toFixed(0)}° mech`} />
            <Readout
              label="Rotor electrical angle"
              value={`${trueElectrical.toFixed(0)}°`}
              hint="Where the magnets really are"
            />
            <Readout
              label="Drive's belief"
              value={`${driveElectrical.toFixed(0)}°`}
              hint="Encoder reading plus the stored offset"
            />
            <Readout label="Error" value={`${error.toFixed(0)}°`} emphasis={!aligned} />
          </div>
        </div>
      </div>
    </div>
  )
}
