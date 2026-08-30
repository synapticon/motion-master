import PolePairSelector from './PolePairSelector'

// A labelled cross-section of a three-phase servo motor, for the Learn pages.
//
// The only thing the reader can change is the pole pair count, because that is the one property of
// the machine that changes what the picture *is* rather than what state it is in. A pole pair is
// one north and one south magnet; a motor with two of them has four magnet poles round the rotor,
// and the winding pattern that drives them repeats twice.
//
// That repetition is the reason the electrical angle is not the shaft angle. The stator sees the
// same U-V-W pattern come round `polePairs` times per turn of the shaft, so the current controller
// goes round its own circle that many times too. Every other Learn figure builds on it, which is
// why this one is where it is introduced.
//
// **The winding drawn here is one layout, not the layout.** Six slots per pole pair, one slot per
// pole per phase, full pitch — the arrangement a textbook starts with. Plenty of real servo motors
// use fractional-slot concentrated windings instead (twelve slots against ten poles, say), which
// look nothing like this and behave the same way for everything this page discusses.
//
// The slot order is the part that is easy to draw wrong. It is not U, V, W repeated: lettering the
// six slots in plain order would put the three phase axes 60 degrees apart instead of the 120 they
// have to be. The sequence is U, W-return, V, U-return, W, V-return, which puts U's axis at 0, V's
// at 120 and W's at 240. A phase's return slot sits one pole pitch away — always 180 electrical
// degrees, which is 180 / polePairs degrees of shaft, so it is directly opposite only on a machine
// with one pole pair. Return slots are drawn a shade lighter.
//
// Colour means one thing here and on the interactive dial: red is a north pole, ocean is a south
// pole. The windings are all the same component as each other, so they are drawn the same and told
// apart by their letters.

const CX = 150
const CY = 150

const rad = (deg: number) => (deg * Math.PI) / 180
const polar = (r: number, deg: number) => ({
  x: CX + r * Math.cos(rad(deg)),
  y: CY - r * Math.sin(rad(deg)),
})

function sector(rInner: number, rOuter: number, fromDeg: number, spanDeg: number) {
  const large = spanDeg > 180 ? 1 : 0
  const a = polar(rOuter, fromDeg)
  const b = polar(rOuter, fromDeg + spanDeg)
  const c = polar(rInner, fromDeg + spanDeg)
  const d = polar(rInner, fromDeg)
  return [
    `M ${a.x.toFixed(2)} ${a.y.toFixed(2)}`,
    `A ${rOuter} ${rOuter} 0 ${large} 0 ${b.x.toFixed(2)} ${b.y.toFixed(2)}`,
    `L ${c.x.toFixed(2)} ${c.y.toFixed(2)}`,
    `A ${rInner} ${rInner} 0 ${large} 1 ${d.x.toFixed(2)} ${d.y.toFixed(2)}`,
    'Z',
  ].join(' ')
}

// One pole pair's worth of slots, in the order they sit round the circle. The whole pattern repeats
// once per pole pair.
const SLOT_PATTERN = ['U×', 'W•', 'V×', 'U•', 'W×', 'V•']

// A caption with a leader line back to the part it names.
function Label({
  text,
  detail,
  atDeg,
  atRadius,
  y,
}: {
  text: string
  detail: string
  atDeg: number
  atRadius: number
  y: number
}) {
  const anchor = polar(atRadius, atDeg)
  return (
    <g>
      <circle cx={anchor.x} cy={anchor.y} r={2.5} className="fill-grey-700" />
      <polyline
        points={`${anchor.x},${anchor.y} ${300},${y} ${312},${y}`}
        className="stroke-grey-400 fill-none"
        strokeWidth={1}
      />
      <text x={318} y={y - 4} className="fill-grey-900 text-[11px]">
        {text}
      </text>
      <text x={318} y={y + 8} className="fill-grey-500 text-[10px]">
        {detail}
      </text>
    </g>
  )
}

export default function MotorCrossSection({
  polePairs,
  onPolePairsChange,
}: {
  polePairs: number
  onPolePairsChange: (polePairs: number) => void
}) {
  const slots = polePairs * 6
  const slotPitch = 360 / slots
  const poles = polePairs * 2
  const poleSpan = 180 / polePairs
  // The letters have to survive twenty-four slots without turning into a smear.
  const phaseFont = Math.max(6, 13 - polePairs * 1.5)

  return (
    <div className="border border-grey-200 max-w-2xl">
      <svg
        viewBox="0 0 560 300"
        className="w-full bg-white"
        role="img"
        aria-label={`Cross-section of a three-phase servo motor with ${polePairs} pole pair${polePairs === 1 ? '' : 's'}: a shaft carrying ${poles} rotor magnet poles, surrounded by ${slots} winding bundles making up the three phases U, V and W.`}
      >
        {/* Housing and stator iron. */}
        <circle cx={CX} cy={CY} r={132} className="fill-grey-50 stroke-grey-300" strokeWidth={1} />
        <circle cx={CX} cy={CY} r={124} className="fill-grey-100 stroke-grey-300" strokeWidth={1} />

        {/* Winding bundles. Six per pole pair, so the U-V-W pattern comes round once per pair. */}
        {Array.from({ length: slots }, (_, i) => {
          const slot = SLOT_PATTERN[i % SLOT_PATTERN.length]
          const centre = i * slotPitch
          const span = slotPitch * 0.78
          const text = polar(108, centre)
          return (
            <g key={i}>
              <path
                d={sector(92, 122, centre - span / 2, span)}
                className="fill-grey-200 stroke-grey-700"
                strokeWidth={1}
              />
              <text
                x={text.x}
                y={text.y}
                className="fill-grey-900 font-mono"
                style={{ fontSize: `${phaseFont}px` }}
                textAnchor="middle"
                dominantBaseline="middle"
              >
                {slot}
              </text>
            </g>
          )
        })}

        {/* Air gap — the stator does not touch the rotor. */}
        <circle
          cx={CX}
          cy={CY}
          r={86}
          className="fill-white stroke-grey-300"
          strokeWidth={1}
          strokeDasharray="3 3"
        />

        {/* Rotor magnets: one north and one south per pole pair. */}
        {Array.from({ length: poles }, (_, i) => (
          <path
            key={i}
            d={sector(20, 80, i * poleSpan, poleSpan)}
            className={i % 2 === 0 ? 'fill-syn-red/85' : 'fill-ocean/85'}
            stroke="white"
            strokeWidth={1}
          />
        ))}
        {polePairs <= 2 &&
          Array.from({ length: poles }, (_, i) => (
            <text
              key={i}
              {...polar(52, i * poleSpan + poleSpan / 2)}
              className="fill-white font-mono"
              style={{ fontSize: polePairs === 1 ? '13px' : '11px' }}
              textAnchor="middle"
              dominantBaseline="middle"
            >
              {i % 2 === 0 ? 'N' : 'S'}
            </text>
          ))}

        {/* Shaft. */}
        <circle cx={CX} cy={CY} r={20} className="fill-grey-800" />
        <circle cx={CX} cy={CY} r={6} className="fill-grey-400" />

        <Label
          text="Stator windings"
          detail="Three phases, U V W. They do not move."
          atDeg={52}
          atRadius={120}
          y={70}
        />
        <Label
          text="Rotor magnets"
          detail="Fixed to the shaft, so they turn with it."
          atDeg={14}
          atRadius={55}
          y={140}
        />
        <Label
          text="Shaft"
          detail="Carries the magnets, the encoder and the load."
          atDeg={-24}
          atRadius={14}
          y={210}
        />
      </svg>

      <div className="border-t border-grey-200 p-4 space-y-4">
        <PolePairSelector value={polePairs} onChange={onPolePairsChange} />
        <div className="text-[11px] text-grey-500 leading-4 space-y-1.5">
          <p>
            <span className="text-grey-900">A pole pair is one north magnet and one south.</span>{' '}
            This rotor has {polePairs}, so {poles} magnet poles.
          </p>
          <p>
            <span className="text-grey-900">
              The winding takes {slots} slots, {slots / 3} for each phase.
            </span>{' '}
            A phase is one long wire that runs down the motor and back, so its slots come in pairs
            and every slot of a phase carries the same current at the same moment. You are looking
            at the motor end-on: <span className="font-mono text-grey-900">×</span> is the wire
            heading away from you, <span className="font-mono text-grey-900">•</span> is the same
            wire coming back.
          </p>
          <p>
            A phase and its return work as one electromagnet, and three phases give three of them
            pointing 120 degrees apart. The next figure shows why a wire and its return reinforce
            each other rather than cancelling; the one in Commutation shows what three of them can
            do together.
          </p>
          <p>
            Add pole pairs and the same pattern of magnets and slots simply repeats round the
            circle, so each phase picks up another pair of slots. It comes round{' '}
            {polePairs === 1 ? 'once' : `${polePairs} times`} per turn of the shaft.
          </p>
          <p>
            A phase&apos;s <span className="font-mono text-grey-900">×</span> and{' '}
            <span className="font-mono text-grey-900">•</span> are always three slots apart, which
            is half of the repeating pattern — {180 / polePairs} degrees of shaft here.{' '}
            {polePairs === 1
              ? 'With one pole pair that is opposite sides of the motor.'
              : 'Two slots facing each other across the motor are a whole pattern apart, so they carry the same mark.'}
          </p>
          <p className="text-grey-400">
            Six slots per pole pair is one common layout. Real motors use others.
          </p>
        </div>
      </div>
    </div>
  )
}
