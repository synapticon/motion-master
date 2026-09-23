import { useState } from 'react'

// The torque-speed envelope, for the Learn pages.
//
// The Torque section explains that back-EMF rises with speed and the supply voltage therefore caps
// it. This is the shape that fact makes, and it is the shape on every servo motor datasheet.
//
// It is a schematic, not a datasheet. Below base speed the current limit sets the torque, so the
// line is flat. Base speed is where the back-EMF has used up the supply, and it moves in proportion
// to that supply, which is what the slider does.
//
// The slider spans what a SOMANET drive actually takes: 14 V minimum, 24-48 V nominal, 58 V
// continuous and 60 V peak. Sizing it to the real range is what makes the figure worth touching —
// a reader can set their own supply and see where their base speed lands. Past it there is no voltage left to push current,
// so torque falls away. Field weakening buys speed past that point by spending current on opposing
// the magnets instead of making torque, which is why the tail is lower rather than longer at the
// same height.

const W = 520
const H = 190
const L = 46
const B = 42

const x = (speed: number) => L + speed * (W - L - 16)
const y = (torque: number) => H - B - torque * (H - B - 16)

export default function TorqueSpeedFigure() {
  const [supply, setSupply] = useState(48)
  const [weakening, setWeakening] = useState(false)

  // Base speed scales with the supply. 48 V nominal is drawn around half the axis so both the
  // low end and the headroom above it have room.
  const base = (supply / 100) * 0.9

  const points: string[] = []
  for (let i = 0; i <= 200; i++) {
    const s = i / 200
    let t: number
    if (s <= base) {
      t = 1
    } else if (weakening) {
      t = base / s
    } else {
      // Without field weakening there is simply no voltage left, and it goes quickly.
      t = Math.max(0, 1 - (s - base) / (base * 0.18))
    }
    points.push(`${i === 0 ? 'M' : 'L'} ${x(s).toFixed(1)} ${y(Math.min(t, 1)).toFixed(1)}`)
  }

  return (
    <div className="border border-grey-200 max-w-2xl">
      <div className="p-4 space-y-4">
        <svg viewBox={`0 0 ${W} ${H}`} className="w-full" role="img"
          aria-label="Torque falling away above base speed, with base speed set by the supply voltage">
          <line x1={L} y1={y(0)} x2={W - 16} y2={y(0)} className="stroke-grey-300" strokeWidth={1} />
          <line x1={L} y1={y(0)} x2={L} y2={12} className="stroke-grey-300" strokeWidth={1} />
          <line x1={L} y1={y(1)} x2={W - 16} y2={y(1)} className="stroke-grey-200" strokeWidth={1} strokeDasharray="3 3" />
          <text x={L - 8} y={y(1) + 3} textAnchor="end" className="fill-grey-500 text-[9px] font-mono">peak</text>
          <text x={L - 8} y={y(0) + 3} textAnchor="end" className="fill-grey-500 text-[9px] font-mono">0</text>

          <line x1={x(base)} y1={y(0)} x2={x(base)} y2={y(1)} className="stroke-grey-300" strokeWidth={1} strokeDasharray="3 3" />
          <text x={x(base)} y={H - 20} textAnchor="middle" className="fill-grey-500 text-[9px]">base speed</text>

          <path d={points.join(' ')} className="stroke-syn-red fill-none" strokeWidth={2.5} />

          {/* Axis titles: the vertical one has to be vertical, or both read as the x axis. */}
          <text
            x={13}
            y={(H - B) / 2 + 8}
            textAnchor="middle"
            transform={`rotate(-90 13 ${(H - B) / 2 + 8})`}
            className="fill-grey-600 text-[11px]"
          >
            torque
          </text>
          <text x={L + (W - L - 16) / 2} y={H - 3} textAnchor="middle" className="fill-grey-600 text-[11px]">
            speed →
          </text>
        </svg>

        <div className="grid sm:grid-cols-2 gap-4 items-end">
          <div>
            <label className="flex items-baseline justify-between text-xs text-grey-700 mb-1.5">
              <span>Supply voltage</span>
              <span className="font-mono text-grey-900">
                {supply} V{supply <= 18 ? ' · near the minimum' : supply >= 50 ? ' · above nominal' : ''}
              </span>
            </label>
            <input
              type="range"
              min={14}
              max={58}
              value={supply}
              onChange={e => setSupply(Number(e.target.value))}
              className="w-full h-1 appearance-none bg-grey-200 accent-syn-red cursor-pointer"
            />
          </div>
          <button
            type="button"
            onClick={() => setWeakening(w => !w)}
            aria-pressed={weakening}
            className={`h-[38px] inline-flex items-center justify-center px-3 text-xs border transition-colors cursor-pointer ${
              weakening
                ? 'border-syn-red bg-syn-red text-white'
                : 'border-syn-red text-syn-red hover:bg-syn-red hover:text-white'
            }`}
          >
            Field weakening {weakening ? 'on' : 'off'}
          </button>
        </div>
      </div>

      <div className="border-t border-grey-200 p-4 text-[11px] text-grey-500 leading-4 space-y-1.5">
        <p>
          <span className="text-grey-900">Below base speed the current limit decides the torque</span>,
          so the line is flat and the motor gives everything it has. Base speed is the point where the
          back-EMF has used up the supply. Raise the supply and it moves out in proportion, which is
          the whole of what the slider does.
        </p>
        <p>
          Past that point there is no voltage left over to push current through the winding, and the
          torque goes quickly. <strong>Field weakening</strong> buys speed past it by spending some
          of the current on opposing the magnets rather than on making torque — the motor keeps
          turning faster, with less to give at every step.
        </p>
        <p>
          The slider covers what a SOMANET drive takes: 14 V at the bottom, 24 to 48 V nominal, and
          58 V continuous with 60 V peak at the top. Below about 12 V a drive will power up and let
          you configure it but will not turn a motor.
        </p>
        <p>
          A shape, not a datasheet. The real curve depends on the winding, the temperature and how
          long you are asking for peak rather than rated current.
        </p>
      </div>
    </div>
  )
}
