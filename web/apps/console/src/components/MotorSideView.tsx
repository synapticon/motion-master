import { Link } from 'react-router'
// A side view of the same motor, for the Learn pages.
//
// It exists because the cross-section cannot answer "where is the encoder". A cross-section is a
// slice straight through the magnets, and the encoder is not in that slice — it is further along
// the shaft. Two views of one machine, with a section marker on this one saying where the other was
// taken.
//
// **What matters is not where along the shaft it sits.** The encoder can be at the back, at the
// front, or built into the middle of the machine, and some motors carry more than one. The only
// property this page depends on is that the disc is rigid with the rotor, so the angle between the
// disc's zero mark and the magnets is fixed once the motor is built and never changes again while
// it runs. The layout drawn here — encoder at the back, load at the front — is the common one, not
// the only one.

// A caption with a leader line back to the part it names.
function Label({
  text,
  detail,
  from,
  to,
  anchor = 'middle',
}: {
  text: string
  detail?: string
  from: { x: number; y: number }
  to: { x: number; y: number }
  anchor?: 'start' | 'middle' | 'end'
}) {
  return (
    <g>
      <circle cx={from.x} cy={from.y} r={2.5} className="fill-grey-700" />
      <line
        x1={from.x}
        y1={from.y}
        x2={to.x}
        y2={to.y}
        className="stroke-grey-400"
        strokeWidth={1}
      />
      <text x={to.x} y={to.y + (to.y < from.y ? -14 : 14)} textAnchor={anchor} className="fill-grey-900 text-[11px]">
        {text}
      </text>
      {detail && (
        <text x={to.x} y={to.y + (to.y < from.y ? -3 : 25)} textAnchor={anchor} className="fill-grey-500 text-[10px]">
          {detail}
        </text>
      )}
    </g>
  )
}

export default function MotorSideView() {
  return (
    <div className="border border-grey-200 max-w-2xl">
      <svg
        viewBox="0 0 560 260"
        className="w-full bg-white"
        role="img"
        aria-label="Side view of a servo motor: the load on the shaft at the front, the rotor magnets and stator windings in the middle, and the encoder disc and its read head on the same shaft at the back."
      >
        {/* Shaft, running the whole length. Everything else is fixed to it or fixed around it. */}
        <rect x={40} y={118} width={400} height={16} className="fill-grey-800" />

        {/* Front end: whatever the motor drives. */}
        <rect x={44} y={92} width={26} height={68} className="fill-grey-400 stroke-grey-700" strokeWidth={1} />

        {/* Motor body. */}
        <rect x={150} y={62} width={190} height={128} className="fill-grey-50 stroke-grey-700" strokeWidth={1} />
        <rect x={158} y={70} width={174} height={22} className="fill-grey-300 stroke-grey-700" strokeWidth={1} />
        <rect x={158} y={160} width={174} height={22} className="fill-grey-300 stroke-grey-700" strokeWidth={1} />
        <rect x={166} y={100} width={158} height={26} className="fill-syn-red/85" />
        <rect x={166} y={126} width={158} height={26} className="fill-ocean/85" />

        {/* Encoder, on the same shaft at the back. The disc turns; the read head does not. */}
        <rect x={368} y={70} width={72} height={112} className="fill-none stroke-grey-300" strokeWidth={1} />
        <rect x={396} y={78} width={7} height={96} className="fill-grey-300 stroke-grey-700" strokeWidth={1} />
        <rect x={386} y={80} width={28} height={12} className="fill-grey-700" />
        <line x1={400} y1={92} x2={400} y2={100} className="stroke-grey-500" strokeWidth={1} strokeDasharray="2 2" />

        {/* Section marker: where the cross-section was cut. */}
        <line
          x1={245}
          y1={38}
          x2={245}
          y2={230}
          className="stroke-syn-red"
          strokeWidth={1}
          strokeDasharray="8 3 2 3"
        />
        <text x={245} y={30} textAnchor="middle" className="fill-syn-red text-[11px] font-mono">
          A
        </text>
        <text x={245} y={246} textAnchor="middle" className="fill-syn-red text-[11px] font-mono">
          A
        </text>

        <Label
          text="Load"
          detail="What the motor drives"
          from={{ x: 57, y: 126 }}
          to={{ x: 57, y: 196 }}
        />
        <Label
          text="Stator windings"
          from={{ x: 200, y: 81 }}
          to={{ x: 128, y: 48 }}
          anchor="start"
        />
        <Label
          text="Rotor magnets"
          from={{ x: 200, y: 126 }}
          to={{ x: 128, y: 206 }}
          anchor="start"
        />
        <Label
          text="Encoder disc"
          detail="Turns with the shaft"
          from={{ x: 399, y: 160 }}
          to={{ x: 420, y: 200 }}
          anchor="middle"
        />
        <Label
          text="Read head"
          detail="Fixed to the housing"
          from={{ x: 400, y: 86 }}
          to={{ x: 548, y: 50 }}
          anchor="end"
        />
      </svg>

      <p className="border-t border-grey-200 p-4 text-[11px] text-grey-500 leading-4">
        The{' '}
        <Link
          to="/learn/servo-motors#what-is-inside"
          className="text-syn-red hover:text-ocean transition-colors"
        >
          cross-section on Servo Motors
        </Link>{' '}
        is the slice at <span className="font-mono text-syn-red">A—A</span>.
        The encoder is not in it, which is why it does not appear there. Where along the shaft the
        encoder sits does not matter to any of this. What matters is that the magnets and the disc
        sit on the same shaft and turn together. The angle between the disc&apos;s zero mark and the
        magnets is fixed when the motor is built and never changes while it runs.
      </p>
    </div>
  )
}
