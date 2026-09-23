import { useId, useState } from 'react'
import Checkbox from './Checkbox'
import PolePairSelector from './PolePairSelector'

// A labelled cross-section of a three-phase servo motor, for the Learn pages.
//
// The reader changes two things. The pole pair count changes what the picture *is*. A pole pair is
// one north and one south magnet, so a motor with two of them has four magnet poles round the
// rotor, and the winding pattern that drives them repeats twice. The slider changes what state the
// picture is in. It sets the electrical angle, and the phase currents and the rotor follow it.
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
// at 120 and W's at 240. A phase's return slot sits one pole pitch away, always 180 electrical
// degrees, which is 180 / polePairs degrees of shaft, so it is directly opposite only on a machine
// with one pole pair. A return slot is marked with a prime: U′ is the other end of U's wire.
//
// PHASE_AXIS gives each phase's electrical position, which is not the direction it pulls. A phase's
// magnetic axis is at right angles to the line joining its two slots, so U at full current, with
// its slots at 0 and 180 degrees, points the field straight up. The rotor is drawn to match: magnet
// zero spans 0 to 180 / polePairs degrees, so its centre sits at 90 / polePairs, which is exactly
// where the field points at zero electrical degrees. That holds at every pole pair count, so the
// rotor starts aligned with the field and stays aligned as the slider turns. Do not "correct" the
// rotor's starting angle without redoing that check.
//
// The × and • marks are live, and that is a deliberate choice. They are the standard notation for a
// vector through the page — × the tail of an arrow going away, • the point of one coming towards
// you — so on a wire cross-section they read as current direction. Drawing them from the winding
// instead would make them wrong for half of every cycle, since the current in a slot reverses twice
// per electrical turn. The winding is what the letter and the prime say.
//
// Colour means one thing here and on the interactive dial: red is a north pole, ocean is a south
// pole. That covers the stator's poles as well as the rotor's, because a pole the current makes is
// still a pole. The windings are all the same component as each other, so they are drawn the same
// and told apart by their letters.
//
// The stator's poles sit where the field the drive sets crosses the air gap: (90 + θ) / polePairs,
// then every pole span round from there. At the first one the field leaves the rotor and enters
// the stator, so that stator pole is a south, and the poles alternate from there. The letters
// outside the housing name the pole. With no lead angle each rotor magnet sits under the opposite
// stator pole, which is the attraction the page describes.
//
// The field lines are computed, not drawn. Three sine currents make a field in the bore whose
// lines are the curves where r^p · sin(p · (φ − ψ)) is constant, with p the pole pair count and ψ
// the first stator pole. With one pole pair that gives straight parallel lines. With more it gives
// arcs from one pole to the next. Outside the bore each line runs straight out to the outer ring
// and closes along it, and those parts are drawn under the slots, because the real flux takes the
// iron between them. The picture is idealised: perfect iron, and a rotor field the same shape as the
// stator's, which is exact only with no lead angle.
//
// Current needs a channel of its own, and it cannot be either of those two, because a slot washed
// red or ocean would read as a magnet. So a slot carrying current is washed yellow, and the depth
// of the wash is the size of the current. The wash says nothing about direction, because the mark
// already says it. A second hue for the other direction would have to be blue, which sits too close
// to ocean to read as anything but a south pole.
//
// All three phases carry current at once. The three are cos(θ), cos(θ − 120) and cos(θ − 240), so
// only one of them is ever at zero, and the figure has to show that: a reader who sees one phase
// lit at a time has learnt six-step switching, which is not what these drives do.
//
// The rotor follows the field with no lead angle, which is the unloaded case. Producing torque
// needs the field held ahead of the magnets, and that belongs to the Commutation Offset page.

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

// Where each phase's axis sits in electrical degrees. The current in a phase is the cosine of the
// angle between the field the drive wants and that axis.
const PHASE_AXIS: Record<string, number> = { U: 0, V: 120, W: 240 }

// The peak phase current the plot is drawn for. A round number, because the shape is the point and
// no real motor is implied by it.
const PEAK_AMPS = 10

// The current plot. One cosine per phase, a third of a cycle apart, with the slider's angle marked.
const PLOT_W = 520
const PLOT_H = 104
const PLOT_LEFT = 30
const PLOT_RIGHT = 12
const PLOT_MID = 52
const PLOT_AMP = 34

const plotX = (deg: number) => PLOT_LEFT + (deg / 360) * (PLOT_W - PLOT_LEFT - PLOT_RIGHT)
const plotY = (share: number) => PLOT_MID - share * PLOT_AMP

function phaseCurve(axisDeg: number) {
  const points = []
  for (let deg = 0; deg <= 360; deg += 4) {
    points.push(`${plotX(deg).toFixed(1)},${plotY(Math.cos(rad(deg - axisDeg))).toFixed(1)}`)
  }
  return `M ${points.join(' L ')}`
}

// The signed current in one slot, from −1 to 1, where positive flows into the page. A phase's
// return slot carries the same current the other way, so the mark decides the sign.
function slotCurrent(slot: string, electricalDeg: number) {
  const phase = PHASE_AXIS[slot[0]]
  const current = Math.cos(rad(electricalDeg - phase))
  return slot[1] === '×' ? current : -current
}

// The field lines: where they cross the bore, and the radii of the outer ring they close along.
const BORE = 86
const RING_INNER = 125
const RING_OUTER = 131

interface FieldLine {
  /** The part inside the bore, from the stator N it leaves to the stator S it enters. */
  bore: string
  /** The part in the stator, from that S back round to that N. */
  stator: string
  /** An arrowhead at the middle of the bore part, pointing the way the field runs. */
  arrow: string
}

function fieldLines(polePairs: number, electricalDeg: number): FieldLine[] {
  const firstPole = (90 + electricalDeg) / polePairs
  // Fewer lines per pole as the poles multiply, or four pole pairs turn into a thicket.
  const levels = polePairs <= 2 ? [0.2, 0.5, 0.8] : [0.3, 0.7]
  const steps = 40
  const lines: FieldLine[] = []
  // Each region k lies between stator pole k and pole k + 1, and holds the lines joining them.
  for (let k = 0; k < polePairs * 2; k++) {
    for (const level of levels) {
      const from = k * Math.PI + Math.asin(level)
      const to = (k + 1) * Math.PI - Math.asin(level)
      const angles: number[] = []
      const points = Array.from({ length: steps + 1 }, (_, i) => {
        const u = from + ((to - from) * i) / steps
        const radius = BORE * Math.pow(level / Math.abs(Math.sin(u)), 1 / polePairs)
        const angle = firstPole + (u * 180) / Math.PI / polePairs
        angles.push(angle)
        return polar(radius, angle)
      })
      // Pole k is a south when k is even. Inside the bore the field runs from the N to the S, so
      // an even region is walked backwards.
      if (k % 2 === 0) {
        points.reverse()
        angles.reverse()
      }
      const fromN = angles[0]
      const intoS = angles[steps]
      // The deepest lines take the widest way round, so they close on the outermost radius and no
      // two lines cross.
      const ring = RING_OUTER - level * (RING_OUTER - RING_INNER)
      const sweep = fromN > intoS ? 0 : 1
      const ringS = polar(ring, intoS)
      const ringN = polar(ring, fromN)
      const bore = `M ${points.map(q => `${q.x.toFixed(1)},${q.y.toFixed(1)}`).join(' L ')}`
      const stator = [
        `M ${points[steps].x.toFixed(1)},${points[steps].y.toFixed(1)}`,
        `L ${ringS.x.toFixed(1)},${ringS.y.toFixed(1)}`,
        `A ${ring} ${ring} 0 0 ${sweep} ${ringN.x.toFixed(1)},${ringN.y.toFixed(1)}`,
        `L ${points[0].x.toFixed(1)},${points[0].y.toFixed(1)}`,
      ].join(' ')
      const mid = points[steps / 2]
      const ahead = points[steps / 2 + 1]
      const heading = Math.atan2(ahead.y - mid.y, ahead.x - mid.x)
      const tip = { x: mid.x + 4 * Math.cos(heading), y: mid.y + 4 * Math.sin(heading) }
      const wing = (side: number) => ({
        x: mid.x - 3 * Math.cos(heading) + side * 3 * Math.sin(heading),
        y: mid.y - 3 * Math.sin(heading) - side * 3 * Math.cos(heading),
      })
      const left = wing(1)
      const right = wing(-1)
      const arrow = `${tip.x.toFixed(1)},${tip.y.toFixed(1)} ${left.x.toFixed(1)},${left.y.toFixed(1)} ${right.x.toFixed(1)},${right.y.toFixed(1)}`
      lines.push({ bore, stator, arrow })
    }
  }
  return lines
}

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
  const [electricalDeg, setElectricalDeg] = useState(0)
  const sliderId = useId()
  const [showField, setShowField] = useState(true)

  const slots = polePairs * 6
  const slotPitch = 360 / slots
  const poles = polePairs * 2
  const poleSpan = 180 / polePairs
  // The rotor keeps step with the field, and the field goes round once per pole pair per turn. So
  // the shaft turns by a fraction of what the drive commands, which is the whole point of the two
  // angles.
  const shaftDeg = electricalDeg / polePairs
  const lines = showField ? fieldLines(polePairs, electricalDeg) : []
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

        {/* The field lines' stator part, under the slots, as the flux runs through the iron. */}
        {lines.map((line, i) => (
          <path key={i} d={line.stator} className="stroke-grey-500 fill-none" strokeWidth={1} />
        ))}

        {/* Winding bundles. Six per pole pair, so the U-V-W pattern comes round once per pair. */}
        {Array.from({ length: slots }, (_, i) => {
          const slot = SLOT_PATTERN[i % SLOT_PATTERN.length]
          const current = slotCurrent(slot, electricalDeg)
          const centre = i * slotPitch
          const span = slotPitch * 0.78
          return (
            <g key={i}>
              <path
                d={sector(92, 122, centre - span / 2, span)}
                className="fill-grey-200 stroke-grey-700"
                strokeWidth={1}
              />
              <path
                d={sector(92, 122, centre - span / 2, span)}
                className="fill-status-warn"
                fillOpacity={Math.abs(current) * 0.75}
                stroke="none"
              />
              <text
                {...polar(113, centre)}
                className="fill-grey-900 font-mono"
                style={{ fontSize: `${phaseFont}px` }}
                textAnchor="middle"
                dominantBaseline="middle"
              >
                {slot[0]}
                {slot[1] === '•' ? '′' : ''}
              </text>
              <text
                {...polar(100, centre)}
                className="fill-grey-700 font-mono"
                style={{ fontSize: `${phaseFont}px` }}
                textAnchor="middle"
                dominantBaseline="middle"
              >
                {current >= 0 ? '×' : '•'}
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

        {/* Rotor magnets: one north and one south per pole pair. They turn with the shaft. SVG
            rotates clockwise and `polar` measures anticlockwise, hence the negative angle. */}
        <g transform={`rotate(${-shaftDeg} ${CX} ${CY})`}>
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
                transform={`rotate(${shaftDeg} ${polar(52, i * poleSpan + poleSpan / 2).x} ${polar(52, i * poleSpan + poleSpan / 2).y})`}
              >
                {i % 2 === 0 ? 'N' : 'S'}
              </text>
            ))}

          {/* Shaft. */}
          <circle cx={CX} cy={CY} r={20} className="fill-grey-800" />
          <circle cx={CX} cy={CY} r={6} className="fill-grey-400" />
        </g>

        {/* The field lines' bore part, over the rotor. */}
        {lines.map((line, i) => (
          <g key={i}>
            <path d={line.bore} className="stroke-grey-900 fill-none" strokeWidth={1} strokeOpacity={0.4} />
            <polygon points={line.arrow} className="fill-grey-900" fillOpacity={0.5} />
          </g>
        ))}

        {/* The stator's poles. */}
        {Array.from({ length: poles }, (_, j) => {
          const at = (90 + electricalDeg) / polePairs + j * poleSpan
          const outward = j % 2 === 0
          return (
            <g key={j}>
              <text
                {...polar(142, at)}
                className={`font-mono font-semibold ${outward ? 'fill-ocean' : 'fill-syn-red'}`}
                style={{ fontSize: '11px' }}
                textAnchor="middle"
                dominantBaseline="middle"
              >
                {outward ? 'S' : 'N'}
              </text>
            </g>
          )
        })}

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

        <label className="inline-flex items-center gap-2 text-xs text-grey-700 cursor-pointer">
          <Checkbox checked={showField} onChange={setShowField} />
          Show field lines
        </label>

        <div>
          <label
            htmlFor={sliderId}
            className="flex items-baseline justify-between text-xs text-grey-700 mb-1.5"
          >
            <span>Field direction the drive sets</span>
            <span className="font-mono text-grey-900">
              {electricalDeg}° electrical, {shaftDeg.toFixed(0)}° of shaft
            </span>
          </label>
          <input
            id={sliderId}
            type="range"
            min={0}
            max={359}
            step={1}
            value={electricalDeg}
            onChange={event => setElectricalDeg(Number(event.target.value))}
            className="w-full h-1 appearance-none bg-grey-200 accent-syn-red cursor-pointer"
          />
        </div>

        <div>
          <svg
            viewBox={`0 0 ${PLOT_W} ${PLOT_H}`}
            className="w-full"
            role="img"
            aria-label={`The current in the three phases against electrical angle. At ${electricalDeg} degrees, phase U carries ${(Math.cos(rad(electricalDeg)) * PEAK_AMPS).toFixed(1)} amps, phase V ${(Math.cos(rad(electricalDeg - 120)) * PEAK_AMPS).toFixed(1)}, phase W ${(Math.cos(rad(electricalDeg - 240)) * PEAK_AMPS).toFixed(1)}.`}
          >
            <line
              x1={PLOT_LEFT}
              y1={plotY(0)}
              x2={PLOT_W - PLOT_RIGHT}
              y2={plotY(0)}
              className="stroke-grey-300"
              strokeWidth={1}
            />
            {[1, -1].map(share => (
              <text
                key={share}
                x={PLOT_LEFT - 6}
                y={plotY(share)}
                textAnchor="end"
                dominantBaseline="middle"
                className="fill-grey-500 font-mono text-[9px]"
              >
                {share > 0 ? PEAK_AMPS : -PEAK_AMPS}
              </text>
            ))}

            {/* One curve per phase. They are told apart by their letters, the way the slots are. */}
            {Object.entries(PHASE_AXIS).map(([letter, axis], i) => (
              <g key={letter}>
                <path
                  d={phaseCurve(axis)}
                  className={['stroke-grey-900', 'stroke-grey-600', 'stroke-grey-400'][i]}
                  fill="none"
                  strokeWidth={1.5}
                />
                <text
                  x={plotX(axis)}
                  y={8}
                  textAnchor="middle"
                  dominantBaseline="middle"
                  className="fill-grey-600 font-mono text-[9px]"
                >
                  {letter}
                </text>
              </g>
            ))}

            {/* Where the slider is. */}
            <line
              x1={plotX(electricalDeg)}
              y1={plotY(1.1)}
              x2={plotX(electricalDeg)}
              y2={plotY(-1.35)}
              className="stroke-grey-400"
              strokeWidth={1}
              strokeDasharray="3 3"
            />
            {Object.values(PHASE_AXIS).map(axis => {
              const share = Math.cos(rad(electricalDeg - axis))
              return (
                <circle
                  key={axis}
                  cx={plotX(electricalDeg)}
                  cy={plotY(share)}
                  r={3.5}
                  className="fill-status-warn stroke-grey-700"
                  strokeWidth={1}
                />
              )
            })}
          </svg>

          <div className="flex gap-4 mt-1">
            {Object.entries(PHASE_AXIS).map(([letter, axis]) => {
              const amps = Math.cos(rad(electricalDeg - axis)) * PEAK_AMPS
              return (
                <span
                  key={letter}
                  className="font-mono text-[11px] text-grey-600 inline-flex items-center gap-1.5"
                >
                  <span
                    className="inline-block h-2 w-2 bg-status-warn"
                    style={{ opacity: Math.abs(amps) / PEAK_AMPS }}
                  />
                  {letter}
                  <span className="text-grey-900">
                    {amps >= 0 ? '+' : ''}
                    {amps.toFixed(1)} A
                  </span>
                </span>
              )
            })}
          </div>
        </div>

        <div className="text-[11px] text-grey-500 leading-4 space-y-1.5">
          <p>
            <span className="text-grey-900">Drag the slider and the shaft turns.</span> The{' '}
            <span className="inline-block h-2 w-2 align-middle bg-status-warn" /> wash on a slot is
            the size of the current in it, deeper for a larger current. The mark under the letter is
            the direction. All three phases carry current at once, because the drive sets all
            three together. The magnets follow the field round.
          </p>
          <p>
            <span className="text-grey-900">You are looking at the motor end-on.</span> The wires
            run along the motor, so each slot shows where they pass through this slice.{' '}
            <span className="font-mono text-grey-900">×</span> is current going away from you and{' '}
            <span className="font-mono text-grey-900">•</span> is current coming towards you. Both
            flip twice per electrical turn, because the current in a winding reverses.
          </p>
          <p>
            <span className="text-grey-900">
              The winding takes {slots} slots, {slots / 3} for each phase.
            </span>{' '}
            A phase is one long wire that runs down the motor and back, so its slots come in pairs
            carrying the same current at the same moment, one way down and the other way back.{' '}
            <span className="font-mono text-grey-900">U</span> is where the wire goes down and{' '}
            <span className="font-mono text-grey-900">U′</span> is where it comes back, which is why
            a phase&apos;s two slots always carry opposite marks. The letter never changes, because
            it names the wire.
          </p>
          <p>
            <span className="text-grey-900">The marks form groups.</span> Every slot in a group
            carries current the same way. Two neighbouring groups act as one big loop, and the field
            goes through its middle.
          </p>
          <p>
            <span className="text-grey-900">The grey lines are the field.</span> Each one is a
            closed loop. It comes out of the stator at an N, crosses the gap and the rotor, goes back
            into the stator at an S, and returns through the outer ring. The arrows show which way it
            runs. The letters outside the housing are the stator&apos;s poles:{' '}
            <span className="font-mono text-syn-red">N</span> where the field comes out of the
            stator, <span className="font-mono text-ocean">S</span> where it goes back in. They go
            round with the slider while the windings stay put, and each rotor magnet sits under the
            opposite stator pole. The lines are idealised. They show the shape of the field with
            perfect iron, and with the rotor lined up with the field, as it is here.
          </p>
          <p>
            The plot is those three currents against electrical angle, each a third of a cycle from
            the next. That is what <span className="text-grey-900">three-phase</span> means. The
            dashed line is where the slider sits, and the dots on it are the three currents at that
            instant, the same currents the slots show. Drawn for a 10 A peak, which is a
            round number rather than any particular motor.
          </p>
          <p>
            <span className="text-grey-900">A pole pair is one north magnet and one south.</span>{' '}
            This rotor has {polePairs}, so {poles} magnet poles.
          </p>
          <p>
            A phase and its return are always three slots apart, which is half of the repeating
            pattern, {180 / polePairs} degrees of shaft here.{' '}
            {polePairs === 1
              ? 'With one pole pair that is opposite sides of the motor.'
              : 'Two slots facing each other across the motor are a whole pattern apart, so they carry the same current the same way.'}
          </p>
          <p className="text-grey-400">
            Six slots per pole pair is one common layout. Others look different and work the same
            way.
          </p>
        </div>
      </div>
    </div>
  )
}
