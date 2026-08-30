import { useEffect, useState } from 'react'
import { Pause, Play } from 'lucide-react'

// Six-step commutation against smooth commutation, for the Learn pages.
//
// The Encoders page states that halls give six states per electrical cycle and therefore block
// commutation, while an encoder gives smooth control. Both halves of that are asserted and neither
// is shown, which leaves the reader with no reason to care how fine the feedback is.
//
// The comparison is exact rather than illustrative. A six-step drive knows only which 60-degree
// sector the rotor is in, so it puts the field at that sector's centre. The error therefore ramps
// from -30 to +30 degrees and back, and torque follows cos of it, dipping to cos(30) = 86.6% at each
// sector boundary and returning to 100% in the middle. That dip, six times per electrical cycle, is
// the torque ripple.

const rad = (deg: number) => (deg * Math.PI) / 180
const DIAL = 118
const DC = DIAL / 2
const R = 46

const polar = (r: number, deg: number) => ({ x: DC + r * Math.cos(rad(deg)), y: DC - r * Math.sin(rad(deg)) })

// What a six-step drive believes: the centre of the 60-degree sector the halls report.
const sectorCentre = (deg: number) => Math.floor(((deg % 360) + 360) % 360 / 60) * 60 + 30

function Dial({ rotor, field, label, tint }: { rotor: number; field: number; label: string; tint: string }) {
  const a = polar(R, rotor)
  const b = polar(R - 6, field)
  const tip = polar(R + 6, field)
  const l = polar(R - 6, field + 7)
  const r = polar(R - 6, field - 7)
  return (
    <div className="text-center">
      <svg viewBox={`0 0 ${DIAL} ${DIAL}`} className="w-full max-w-[130px] mx-auto" role="img" aria-label={label}>
        <circle cx={DC} cy={DC} r={R + 8} className="fill-grey-50 stroke-grey-200" strokeWidth={1} />
        <line x1={DC} y1={DC} x2={a.x} y2={a.y} className="stroke-grey-900" strokeWidth={2} strokeLinecap="round" />
        <line x1={DC} y1={DC} x2={b.x} y2={b.y} className={tint} strokeWidth={2.5} strokeLinecap="round" />
        <polygon points={`${tip.x},${tip.y} ${l.x},${l.y} ${r.x},${r.y}`} className={tint.replace('stroke-', 'fill-')} />
        <circle cx={DC} cy={DC} r={3} className="fill-grey-800" />
      </svg>
      <p className="text-[11px] text-grey-600 mt-1">{label}</p>
    </div>
  )
}

export default function CommutationCompareFigure() {
  const [angle, setAngle] = useState(20)
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
      setAngle(a => (a + seconds * 50) % 360)
      frame = requestAnimationFrame(step)
    }
    frame = requestAnimationFrame(step)
    return () => cancelAnimationFrame(frame)
  }, [spinning])

  const blockError = sectorCentre(angle) - angle
  const blockTorque = Math.cos(rad(blockError))

  // Torque against electrical angle, one full cycle, for the plot below.
  const PW = 520
  const PH = 96
  const px = (deg: number) => 40 + (deg / 360) * (PW - 56)
  const py = (t: number) => PH - 14 - (t - 0.8) / 0.22 * (PH - 30)
  const blockPath = Array.from({ length: 361 }, (_, d) => {
    const t = Math.cos(rad(sectorCentre(d) - d))
    return `${d === 0 ? 'M' : 'L'} ${px(d).toFixed(1)} ${py(t).toFixed(1)}`
  }).join(' ')

  return (
    <div className="border border-grey-200 max-w-2xl">
      <div className="p-4 space-y-4">
        <div className="grid grid-cols-2 gap-6 max-w-sm mx-auto">
          <Dial rotor={angle} field={sectorCentre(angle) + 90} label="From halls" tint="stroke-status-warn" />
          <Dial rotor={angle} field={angle + 90} label="From an encoder" tint="stroke-ocean" />
        </div>
        <p className="text-[11px] text-grey-500 leading-4 text-center max-w-sm mx-auto">
          Black is the rotor. The coloured arrow is where the drive puts its current.
        </p>

        <svg viewBox={`0 0 ${PW} ${PH}`} className="w-full" role="img"
          aria-label="Torque against electrical angle for six-step and smooth commutation">
          <line x1={40} y1={py(1)} x2={PW - 16} y2={py(1)} className="stroke-grey-200" strokeWidth={1} />
          <line x1={40} y1={py(0.866)} x2={PW - 16} y2={py(0.866)} className="stroke-grey-200" strokeWidth={1} strokeDasharray="3 3" />
          <text x={34} y={py(1) + 3} textAnchor="end" className="fill-grey-500 text-[9px] font-mono">100%</text>
          <text x={34} y={py(0.866) + 3} textAnchor="end" className="fill-grey-500 text-[9px] font-mono">87%</text>

          <path d={blockPath} className="stroke-status-warn fill-none" strokeWidth={2} />
          <line x1={40} y1={py(1)} x2={PW - 16} y2={py(1)} className="stroke-ocean" strokeWidth={2} />

          <line x1={px(angle)} y1={4} x2={px(angle)} y2={PH - 12} className="stroke-grey-400" strokeWidth={1} strokeDasharray="3 3" />
          <circle cx={px(angle)} cy={py(blockTorque)} r={3.5} className="fill-status-warn" />
          <circle cx={px(angle)} cy={py(1)} r={3.5} className="fill-ocean" />
          <text x={40} y={PH - 2} className="fill-grey-500 text-[9px]">0°</text>
          <text x={PW - 16} y={PH - 2} textAnchor="end" className="fill-grey-500 text-[9px]">
            360° electrical
          </text>
        </svg>

        <div className="grid sm:grid-cols-2 gap-4 items-end">
          <div>
            <label className="flex items-baseline justify-between text-xs text-grey-700 mb-1.5">
              <span>Rotor electrical angle</span>
              <span className="font-mono text-grey-900">{angle.toFixed(0)}°</span>
            </label>
            <input
              type="range"
              min={0}
              max={359}
              value={Math.round(angle)}
              onChange={e => {
                setSpinning(false)
                setAngle(Number(e.target.value))
              }}
              className="w-full h-1 appearance-none bg-grey-200 accent-syn-red cursor-pointer"
            />
          </div>
          <div className="flex items-center gap-3">
            <button
              type="button"
              onClick={() => setSpinning(s => !s)}
              className="h-[38px] inline-flex items-center gap-1.5 px-3 text-xs border border-syn-red text-syn-red hover:bg-syn-red hover:text-white transition-colors cursor-pointer"
            >
              {spinning ? <Pause className="h-3.5 w-3.5" aria-hidden /> : <Play className="h-3.5 w-3.5" aria-hidden />}
              {spinning ? 'Stop' : 'Turn the rotor'}
            </button>
            <p className="text-[11px] text-grey-600 leading-4 font-mono">
              halls {(blockTorque * 100).toFixed(0)}%
              <span className="block">encoder 100%</span>
            </p>
          </div>
        </div>
      </div>

      <div className="border-t border-grey-200 p-4 text-[11px] text-grey-500 leading-4 space-y-1.5">
        <p>
          <span className="text-grey-900">Halls report a sector, not an angle.</span> The best a drive
          can do with that is point its current at the middle of the sector and leave it there until
          the rotor crosses into the next one. So the field waits, jumps 60 degrees, and waits again
          — six positions per electrical cycle, which is where <strong>six-step</strong> comes from.
        </p>
        <p>
          The error that leaves ramps from −30 to +30 degrees and back, and torque follows the cosine
          of it: full in the middle of each sector, down to 87 percent at every boundary. Six dips per
          electrical cycle. That is <strong>torque ripple</strong>, and at low speed you can feel it.
        </p>
        <p>
          An encoder reports the angle itself, so the field tracks the rotor instead of chasing it,
          the error stays at zero and the torque line is flat. That gap between the two lines is what
          the finer feedback buys.
        </p>
      </div>
    </div>
  )
}
