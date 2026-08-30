import { useState } from 'react'
import { RotateCcw } from 'lucide-react'

// What an incremental encoder actually sends down the wire, for the Learn pages.
//
// Three terms the Encoders page uses and never shows: the two channels, which one leads, and the
// index. All three are visible at once here, and each explains a setting a drive asks about.
//
// The drawing uses four cycles per turn so that one whole turn and the individual edges both fit on
// screen. A real encoder puts thousands of cycles in the same space, which changes the numbers and
// nothing else.

const CYCLES = 4
const INDEX_AT = 0.62
const W = 520
const LEFT = 54
const PLOT = W - LEFT - 12
const ROW_H = 34
const HIGH = 8
const LOW = 26

function Channel({ y, label, phaseShift, position, reversed }: {
  y: number
  label: string
  phaseShift: number
  position: number
  reversed: boolean
}) {
  // One square wave across the whole turn. Reversing direction swaps which channel leads, which is
  // the only thing that tells a counter which way the shaft is going.
  const shift = reversed ? -phaseShift : phaseShift
  const steps: string[] = []
  const samples = 400
  for (let i = 0; i <= samples; i++) {
    const turn = i / samples
    const phase = (turn * CYCLES + shift + 1) % 1
    const high = phase < 0.5
    const x = LEFT + turn * PLOT
    const yy = y + (high ? HIGH : LOW)
    steps.push(`${i === 0 ? 'M' : 'L'} ${x.toFixed(1)} ${yy}`)
  }
  return (
    <g>
      <text x={LEFT - 10} y={y + 20} textAnchor="end" className="fill-grey-700 text-[12px] font-mono">
        {label}
      </text>
      <line x1={LEFT} y1={y + LOW} x2={LEFT + PLOT} y2={y + LOW} className="stroke-grey-200" strokeWidth={1} />
      <path d={steps.join(' ')} className="stroke-ocean fill-none" strokeWidth={2} shapeRendering="crispEdges" />
      <circle
        cx={LEFT + position * PLOT}
        cy={y + (((position * CYCLES + (reversed ? -phaseShift : phaseShift) + 1) % 1) < 0.5 ? HIGH : LOW)}
        r={3.5}
        className="fill-syn-red"
      />
    </g>
  )
}

export default function QuadratureFigure() {
  const [position, setPosition] = useState(0.18)
  const [reversed, setReversed] = useState(false)

  const edges = Math.floor(position * CYCLES * 4)
  const indexPassed = position > INDEX_AT

  return (
    <div className="border border-grey-200 max-w-2xl">
      <div className="p-4">
        <svg viewBox={`0 0 ${W} 150`} className="w-full" role="img"
          aria-label="Two quadrature channels a quarter cycle apart, plus an index pulse once per turn">
          <Channel y={4} label="A" phaseShift={0} position={position} reversed={reversed} />
          <Channel y={4 + ROW_H} label="B" phaseShift={-0.25} position={position} reversed={reversed} />

          {/* Index: one pulse per turn, at the same shaft angle every time. */}
          <g>
            <text x={LEFT - 10} y={4 + 2 * ROW_H + 20} textAnchor="end" className="fill-grey-700 text-[12px] font-mono">
              Z
            </text>
            <line x1={LEFT} y1={4 + 2 * ROW_H + LOW} x2={LEFT + PLOT} y2={4 + 2 * ROW_H + LOW}
              className="stroke-grey-200" strokeWidth={1} />
            <path
              d={`M ${LEFT} ${4 + 2 * ROW_H + LOW} L ${LEFT + INDEX_AT * PLOT} ${4 + 2 * ROW_H + LOW} L ${LEFT + INDEX_AT * PLOT} ${4 + 2 * ROW_H + HIGH} L ${LEFT + INDEX_AT * PLOT + 8} ${4 + 2 * ROW_H + HIGH} L ${LEFT + INDEX_AT * PLOT + 8} ${4 + 2 * ROW_H + LOW} L ${LEFT + PLOT} ${4 + 2 * ROW_H + LOW}`}
              className="stroke-syn-red fill-none" strokeWidth={2} shapeRendering="crispEdges" />
          </g>

          {/* Position cursor across all three rows. */}
          <line x1={LEFT + position * PLOT} y1={0} x2={LEFT + position * PLOT} y2={4 + 3 * ROW_H}
            className="stroke-grey-400" strokeWidth={1} strokeDasharray="3 3" />

          <line x1={LEFT} y1={4 + 3 * ROW_H + 4} x2={LEFT + PLOT} y2={4 + 3 * ROW_H + 4}
            className="stroke-grey-300" strokeWidth={1} />
          <text x={LEFT} y={4 + 3 * ROW_H + 20} className="fill-grey-500 text-[10px]">one turn of the shaft</text>
          <text x={LEFT + PLOT} y={4 + 3 * ROW_H + 20} textAnchor="end" className="fill-grey-500 text-[10px]">
            back to the start
          </text>
        </svg>

        <div className="mt-3 grid sm:grid-cols-2 gap-4 items-end">
          <div>
            <label className="flex items-baseline justify-between text-xs text-grey-700 mb-1.5">
              <span>Shaft position</span>
              <span className="font-mono text-grey-900">{(position * 360).toFixed(0)}°</span>
            </label>
            <input
              type="range"
              min={0}
              max={1000}
              value={Math.round(position * 1000)}
              onChange={e => setPosition(Number(e.target.value) / 1000)}
              className="w-full h-1 appearance-none bg-grey-200 accent-syn-red cursor-pointer"
            />
          </div>
          <div className="flex items-center gap-3">
            <button
              type="button"
              onClick={() => setReversed(r => !r)}
              className="h-[38px] inline-flex items-center gap-1.5 px-3 text-xs border border-syn-red text-syn-red hover:bg-syn-red hover:text-white transition-colors cursor-pointer"
            >
              <RotateCcw className="h-3.5 w-3.5" aria-hidden />
              Turn the other way
            </button>
            <p className="text-[11px] text-grey-600 leading-4">
              {reversed ? 'B leads A' : 'A leads B'}
              <span className="block text-grey-500">
                {edges} edges counted{indexPassed ? ', index crossed' : ', index not reached'}
              </span>
            </p>
          </div>
        </div>
      </div>

      <div className="border-t border-grey-200 p-4 text-[11px] text-grey-500 leading-4 space-y-1.5">
        <p>
          <span className="text-grey-900">A and B are the same square wave a quarter cycle apart.</span>{' '}
          Neither one alone says anything about direction. Together they do: turn one way and A
          changes first, turn the other and B does. That is the whole of it, and it is why a drive has
          a polarity setting — swap the two wires and every count goes the wrong way.
        </p>
        <p>
          A counter that watches every edge on both channels gets four counts per cycle, which is
          where the &ldquo;times four&rdquo; in an encoder&apos;s specification comes from.
        </p>
        <p>
          <span className="text-grey-900">Z is the index</span>, one pulse at the same shaft angle
          every turn. It is the only absolute thing an incremental encoder produces: until the shaft
          has crossed it, the count is a number of steps from wherever the drive happened to switch
          on. That is what a homing move goes looking for.
        </p>
        <p>
          Drawn with four cycles per turn so a whole turn and the individual edges both fit. A real
          encoder puts thousands in the same space.
        </p>
      </div>
    </div>
  )
}
