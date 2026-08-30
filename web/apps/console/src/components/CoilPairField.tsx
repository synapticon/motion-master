// Why the two slots of a phase work as one electromagnet, for the Learn pages.
//
// The claim it illustrates is easy to state and hard to believe on a first read: a wire and its
// return, on opposite sides of the rotor, reinforce each other rather than cancelling. The contrast
// panel is what makes it land — the same two wires carrying current the same way really do cancel,
// so the reinforcement is a property of the loop and not of having two wires.
//
// Directions, checked rather than assumed. Current away from the viewer circles its field
// clockwise, so at a point to its left the field points up. Current toward the viewer circles
// counter-clockwise, so at a point to its right the field also points up. Two wires, one of each,
// with the point between them: both contributions point the same way.

const LEFT = 62
const RIGHT = 178
const AXIS = 92

// A conductor seen end-on, with the standard mark for which way the current runs.
function Conductor({ x, into }: { x: number; into: boolean }) {
  return (
    <g>
      <circle cx={x} cy={AXIS} r={13} className="fill-white stroke-grey-800" strokeWidth={1.5} />
      {into ? (
        <>
          <line x1={x - 6} y1={AXIS - 6} x2={x + 6} y2={AXIS + 6} className="stroke-grey-900" strokeWidth={1.5} />
          <line x1={x - 6} y1={AXIS + 6} x2={x + 6} y2={AXIS - 6} className="stroke-grey-900" strokeWidth={1.5} />
        </>
      ) : (
        <circle cx={x} cy={AXIS} r={3.5} className="fill-grey-900" />
      )}
    </g>
  )
}

// The field wrapped round one conductor. The arrowhead sits at the top of the outer ring, where
// clockwise points right and counter-clockwise points left, so one triangle states the direction.
function FieldRings({ x, clockwise }: { x: number; clockwise: boolean }) {
  const tip = clockwise ? 7 : -7
  return (
    <g className="stroke-grey-300 fill-none">
      <circle cx={x} cy={AXIS} r={26} strokeWidth={1} />
      <circle cx={x} cy={AXIS} r={42} strokeWidth={1} />
      <polygon
        points={`${x + tip},${AXIS - 42} ${x - tip * 0.2},${AXIS - 46.5} ${x - tip * 0.2},${AXIS - 37.5}`}
        className="fill-grey-400 stroke-none"
      />
    </g>
  )
}

function Panel({
  label,
  detail,
  leftInto,
  rightInto,
  adds,
}: {
  label: string
  detail: string
  leftInto: boolean
  rightInto: boolean
  adds: boolean
}) {
  return (
    <div className="p-4">
      <h3 className="text-xs font-medium text-grey-900 mb-1">{label}</h3>
      <p className="text-[11px] text-grey-600 leading-4 mb-2">{detail}</p>
      <svg viewBox="0 0 240 188" className="w-full max-w-[280px] mx-auto" role="img" aria-label={detail}>
        <FieldRings x={LEFT} clockwise={leftInto} />
        <FieldRings x={RIGHT} clockwise={rightInto} />

        {adds ? (
          <g>
            <line x1={120} y1={132} x2={120} y2={62} className="stroke-syn-red" strokeWidth={3} />
            <polygon points="120,50 126,64 114,64" className="fill-syn-red" />
            <text x={120} y={152} textAnchor="middle" className="fill-syn-red text-[10px]">
              fields add
            </text>
          </g>
        ) : (
          <g>
            <line x1={110} y1={128} x2={110} y2={68} className="stroke-grey-500" strokeWidth={2.5} />
            <polygon points="110,58 115,70 105,70" className="fill-grey-500" />
            <line x1={132} y1={68} x2={132} y2={128} className="stroke-grey-500" strokeWidth={2.5} />
            <polygon points="132,138 137,126 127,126" className="fill-grey-500" />
            <text x={121} y={152} textAnchor="middle" className="fill-grey-500 text-[10px]">
              fields oppose
            </text>
          </g>
        )}

        <Conductor x={LEFT} into={leftInto} />
        <Conductor x={RIGHT} into={rightInto} />
        <text x={LEFT} y={AXIS + 84} textAnchor="middle" className="fill-grey-600 text-[10px]">
          {leftInto ? 'away from you' : 'toward you'}
        </text>
        <text x={RIGHT} y={AXIS + 84} textAnchor="middle" className="fill-grey-600 text-[10px]">
          {rightInto ? 'away from you' : 'toward you'}
        </text>
      </svg>
    </div>
  )
}

export default function CoilPairField() {
  return (
    <div className="border border-grey-200 max-w-2xl">
      <div className="grid sm:grid-cols-2 divide-y sm:divide-y-0 sm:divide-x divide-grey-200">
        <Panel
          label="One phase: a wire and its return"
          detail="Current out through one slot, back through the other."
          leftInto={false}
          rightInto
          adds
        />
        <Panel
          label="The same two wires, same direction"
          detail="Not how a phase is wound. Shown for the contrast."
          leftInto
          rightInto
          adds={false}
        />
      </div>
      <div className="border-t border-grey-200 p-4 text-[11px] text-grey-500 leading-4 space-y-1.5">
        <p>
          <span className="text-grey-900">Every wire carrying current is wrapped in a field.</span>{' '}
          Right hand, thumb along the current, fingers curl the way the field goes. The grey rings
          are that field and the small arrow is which way it runs.
        </p>
        <p>
          On the left, the two contributions between the wires point the same way and reinforce. On
          the right they point opposite ways and cancel. So the return leg is not a wire to be
          routed out of the way — it is half the magnet, which is why a winding is a loop and why
          the two slots of a phase belong together.
        </p>
        <p>
          Note where the combined field points: across the gap, at a right angle to the line joining
          the two conductors. A phase&apos;s magnetic axis is perpendicular to its pair of slots,
          not along it.
        </p>
      </div>
    </div>
  )
}
