import { Link } from 'react-router'
import PageHeader from '../components/PageHeader'
import LearnSections from '../components/LearnSections'
import type { LearnSectionEntry } from '../components/LearnSections'
import MotorSideView from '../components/MotorSideView'
import QuadratureFigure from '../components/QuadratureFigure'
import CommutationCompareFigure from '../components/CommutationCompareFigure'

// Learn → Encoders.
//
// Everything about the feedback on the shaft, including the arrangements that are not encoders at
// all. It is its own page because it outgrew Servo Motors and because position control, homing and
// the commissioning procedures all need it independently of how a motor is built.
//
// The one thing it borrows is electrical angle, which is linked at the point it is used rather than
// recapped up front.

export default function LearnEncodersPage() {
  const sections: LearnSectionEntry[] = [
    {
      id: 'where-it-sits',
      title: 'Where the encoder sits',
      tab: 'Where it sits',
      content: (
        <>
          <p>
            The encoder is the sensor that answers &ldquo;where is the rotor&rdquo;. It measures the
            angle of the shaft it is fixed to. What it cannot say is how its own zero relates to the
            magnets, because that was settled when somebody assembled the motor. Two motors off the
            same production line can differ.
          </p>
          <MotorSideView />
        </>
      ),
    },
    {
      id: 'kinds-of-feedback',
      title: 'Kinds of feedback',
      tab: 'Kinds',
      content: (
        <>
          <p>
            Position feedback comes in a few arrangements, and not every motor has an encoder at
            all. Two questions separate them: how finely the angle is reported, and whether the
            signal says <em>where the shaft is</em> or only <em>how far it has moved</em>.
          </p>
          <p>
            That second question is worth stating carefully, because the words for it get used as
            if they named kinds of hardware. <strong>Absolute</strong> and{' '}
            <strong>incremental</strong> describe the signal, not the device. Plenty of encoders
            provide both at once, and an incremental one with an index mark becomes absolute as soon
            as the shaft has passed that mark. Hall sensors are absolute too — they just answer very
            coarsely.
          </p>
          <div className="border border-grey-200 overflow-x-auto max-w-3xl">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  {['', 'Hall sensors', 'Encoder, incremental', 'Encoder, absolute'].map(h => (
                    <th
                      key={h}
                      className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap"
                    >
                      {h}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody className="text-grey-700 align-top">
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 text-grey-900">What it reports</td>
                  <td className="px-4 py-2">Which magnet is passing, from three switches</td>
                  <td className="px-4 py-2">How far the shaft has moved since power-up</td>
                  <td className="px-4 py-2">The shaft angle itself</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 text-grey-900">How finely</td>
                  <td className="px-4 py-2">6 states per electrical cycle</td>
                  <td className="px-4 py-2">Thousands of counts per turn</td>
                  <td className="px-4 py-2">Thousands of counts per turn</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 text-grey-900">Known at power-up</td>
                  <td className="px-4 py-2">The 60-degree sector, straight away</td>
                  <td className="px-4 py-2">Nothing, unless a battery kept the count alive</td>
                  <td className="px-4 py-2">The angle, straight away</td>
                </tr>
                <tr>
                  <td className="px-4 py-2 text-grey-900">Good enough for</td>
                  <td className="px-4 py-2">Block commutation, six steps per cycle</td>
                  <td className="px-4 py-2">Smooth control, once the angle has been re-established</td>
                  <td className="px-4 py-2">Smooth control, immediately</td>
                </tr>
              </tbody>
            </table>
          </div>
          <p>
            Halls are cheap and know something the instant the power comes on, which is enough to
            start a motor turning but not to drive it smoothly. An encoder is fine enough for smooth
            control, but an incremental signal only ever says how far, so unless something kept the
            count alive while the power was off, what it means has to be established again at every
            start.
          </p>
          <QuadratureFigure />
          <p>
            A backup battery is the usual way of keeping it alive. It keeps the counter running while the drive is
            off, so the position survives a power cycle even though the signal is still incremental.
            Worth knowing that this is a maintenance item rather than a property of the encoder: let
            the battery go flat, or unplug the encoder, and the position is gone with it.
          </p>
          <p>
            Absolute encoders come as <strong>singleturn</strong>, which reports the angle within one
            turn of the shaft, and <strong>multiturn</strong>, which also counts whole turns.
            Commutation never needs the multiturn part, because the magnet pattern repeats every
            turn anyway.
          </p>
          <p>
            What multiturn buys is knowing where the <em>machine</em> is, not where the rotor is. A
            ballscrew axis with 300 mm of travel might be fifty turns of the motor, and a joint
            behind a 100:1 gearbox is a hundred. A singleturn encoder cannot tell turn three from
            turn four, so the machine has to go and find a reference every time it powers up: a
            homing move against a limit switch or an index mark, which takes time and needs room to
            move. A multiturn encoder already knows, so the axis is ready the moment it comes on,
            including after a power cut nobody planned.
          </p>
          <p>
            That turn counter has to survive with the power off, so it is kept alive either by the
            same kind of backup battery, or by a self-powered mechanism that needs no supply at all.
            Which one it is decides whether the encoder is a maintenance item.
          </p>
        </>
      ),
    },
    {
      id: 'more-than-one',
      title: 'More than one device',
      tab: 'One or two',
      content: (
        <>
          <p>
            A machine can carry more than one feedback device, and they need not do the same job.
            Commutation needs the angle of the motor shaft itself. Put a gearbox in the way and a
            sensor on the output side turns more slowly than the magnets do, so it cannot say where
            they are. It can still serve position control perfectly well, while a second device on
            the motor shaft serves commutation. A drive that lets you name a commutation sensor and
            a position sensor separately is asking about exactly this.
          </p>
          <p>
            Direction counts as much as position. The feedback has to increase when the motor turns
            the way the drive calls forward. If it counts the other way, the drive&apos;s picture of
            the rotor runs backwards as the rotor runs forwards, and commutation cannot work at all.
            That is why a sensor&apos;s polarity is part of its configuration rather than a detail.
          </p>
        </>
      ),
    },
    {
      id: 'counts-and-resolution',
      title: 'What the encoder actually reports',
      tab: 'Resolution',
      content: (
        <>
          <p>
            Not degrees. An encoder reports a <strong>count</strong>. Its{' '}
            <strong>resolution</strong> is how many distinct counts it can report in one turn of the
            shaft — 65536 of them for a 16-bit singleturn encoder, one every 0.0055 degrees. Every
            angle in degrees on this page is arithmetic the drive does on that count.
          </p>
          <p className="font-mono text-xs bg-grey-50 border border-grey-200 px-4 py-3 leading-5">
            shaft degrees = count × 360 / resolution
            <br />
            electrical degrees = count × 360 × pole pairs / resolution
          </p>
          <p>
            A drive mostly stays in counts and never touches degrees. Counts are integers and the
            wrap at a full turn is a mask rather than a division. Degrees are used here because 70
            degrees means the same thing on every machine and count 12743 does not.
          </p>
          <p>
            Commutation happens in{' '}
            <Link
              to="/learn/servo-motors#shaft-and-electrical-angle"
              className="text-syn-red hover:text-ocean transition-colors"
            >
              electrical angle
            </Link>
            , so what matters is counts per electrical cycle, which is{' '}
            <span className="font-mono">resolution / pole pairs</span>. More pole pairs
            means fewer counts per cycle out of the same encoder. At 65536 counts and 4 pole pairs
            that is 16384 per cycle, or 0.022 electrical degrees a count — far finer than
            commutation needs.
          </p>
          <p>
            Resolution does start to matter at the other end of the range. Halls give six states per
            electrical cycle, so the position is known to 60 electrical degrees and no better. Since{' '}
            <span className="font-mono">cos(30°)</span> is 0.87, commutating from halls alone loses
            torque and ripples by construction. That is the difference between block commutation
            from halls and smooth field-oriented control from an encoder.
          </p>
          <CommutationCompareFigure />
          <p>
            Resolution is not accuracy, and the two are easy to confuse because only one of them
            appears on a datasheet front page. Resolution is how finely the encoder divides the
            circle. Accuracy is whether those divisions sit where they claim to. An encoder can
            report 24 bits and still be a tenth of a degree out from eccentricity or a mounting
            error. Every angle the drive computes inherits the accuracy, not the resolution, so a
            finer encoder does not rescue a badly mounted one.
          </p>
        </>
      ),
    },
    {
      id: 'on-somanet-devices',
      title: 'On SOMANET devices',
      tab: 'SOMANET',
      content: (
        <>
          <p>
            A SOMANET drive reads at most two encoders, and each one gets a record of its own:{' '}
            <span className="font-mono">0x2110</span> for the first and{' '}
            <span className="font-mono">0x2112</span> for the second. They hold the same subitems as
            each other. Beside each sits a read-only twin — <span className="font-mono">0x2111</span>{' '}
            and <span className="font-mono">0x2113</span> — carrying raw position, adjusted position,
            velocity and a status word.
          </p>
          <p>
            <strong>One record has to describe every kind of encoder the drive supports</strong>, so
            it is the union of everything any of them needs, and most of it is beside the point for
            any one encoder. Set the type first and the rest sorts itself out: a BiSS encoder cares
            about frame size and CRC, a sine-wave encoder cares about cycles per revolution, and
            neither cares about the other&apos;s fields.
          </p>
          <div className="border border-grey-200 overflow-x-auto max-w-3xl">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  <th className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">Sub</th>
                  <th className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium">What</th>
                  <th className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium">Note</th>
                </tr>
              </thead>
              <tbody className="text-grey-700">
                <tr className="border-b border-grey-100 bg-grey-50/60"><td colSpan={3} className="px-4 py-1.5 text-grey-900">Whatever the encoder is</td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:01</td><td className="px-4 py-2">Sensor port</td><td className="px-4 py-2">where the drive reads it from</td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:02</td><td className="px-4 py-2">Type</td><td className="px-4 py-2">which of the fields below matter</td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:03</td><td className="px-4 py-2">Resolution</td><td className="px-4 py-2">Inc/Revolution</td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:05</td><td className="px-4 py-2">Polarity</td><td className="px-4 py-2">which way it counts</td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:06</td><td className="px-4 py-2">Singleturn offset</td><td className="px-4 py-2">Inc</td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:04</td><td className="px-4 py-2">Zero velocity threshold</td><td className="px-4 py-2">µs</td></tr>
                <tr className="border-b border-grey-100 bg-grey-50/60"><td colSpan={3} className="px-4 py-1.5 text-grey-900">Serial protocols only</td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:08</td><td className="px-4 py-2">Clock frequency</td><td className="px-4 py-2">kHz</td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:09</td><td className="px-4 py-2">Frame size</td><td className="px-4 py-2"></td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:0A – :0D</td><td className="px-4 py-2">Multiturn and singleturn bits, and where each starts</td><td className="px-4 py-2"></td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:0E, :10, :13</td><td className="px-4 py-2">Timeout, maximum tbusy, first clock delay</td><td className="px-4 py-2">µs</td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:0F, :12</td><td className="px-4 py-2">CRC polynomial, parity type</td><td className="px-4 py-2"></td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:11, :14, :15</td><td className="px-4 py-2">Status bits active value, data ordering, endianness</td><td className="px-4 py-2"></td></tr>
                <tr className="border-b border-grey-100 bg-grey-50/60"><td colSpan={3} className="px-4 py-1.5 text-grey-900">One type each</td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:16</td><td className="px-4 py-2">Index availability</td><td className="px-4 py-2">incremental</td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:17</td><td className="px-4 py-2">Hall sensor port</td><td className="px-4 py-2">halls</td></tr>
                <tr className="border-b border-grey-100 last:border-0"><td className="px-4 py-2 font-mono whitespace-nowrap">:18 – :1A</td><td className="px-4 py-2">Sinewave cycles per revolution, resolution, output voltage</td><td className="px-4 py-2">sine-wave</td></tr>
              </tbody>
            </table>
          </div>
          <p>
            <span className="font-mono">:01</span> says where the drive reads the encoder from. Not
            every encoder arrives on a connector: some SOMANET drives have one built in, and the
            port is where that is selected rather than an external plug.
          </p>
          <p>
            <strong>Configuring an encoder does not put it to work.</strong> That is a separate
            decision, taken once per controller, and it is where the two-devices idea from further up
            this page becomes a setting you can see:
          </p>
          <div className="border border-grey-200 overflow-x-auto max-w-3xl">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  <th className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">Object</th>
                  <th className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium">Encoder source for</th>
                </tr>
              </thead>
              <tbody className="text-grey-700">
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono">0x2010:0C</td>
                  <td className="px-4 py-2">The torque controller — this is the commutation encoder</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono">0x2011:05</td>
                  <td className="px-4 py-2">The velocity controller</td>
                </tr>
                <tr>
                  <td className="px-4 py-2 font-mono">0x2012:09</td>
                  <td className="px-4 py-2">The position controller</td>
                </tr>
              </tbody>
            </table>
          </div>
          <p>
            All three can name the same encoder, and often do. Pointing the torque controller at one
            device and the position controller at another is exactly the gearbox case described
            earlier, and this is the setting that expresses it.
          </p>
          <p>
            <strong>Some encoders can be talked to directly.</strong> They have registers of their
            own, and the drive will pass a read or a write through to them. Under Devices →
            Procedures you will find <strong>Encoder register communication</strong> for that,{' '}
            <strong>Kübler register communication</strong> for Kübler devices, and{' '}
            <strong>iC-MU calibration mode</strong>, which puts an iC-MU encoder into one of its
            calibration modes rather than reading anything.
          </p>
          <p>
            <strong>A few can take a firmware update through the drive.</strong> The Kübler encoders
            can, and it runs as part of <strong>Firmware installation</strong> — the same procedure
            name, a different target from the drive&apos;s own firmware.
          </p>
        </>
      ),
    },
    {
      id: 'where-next',
      title: 'Where this leads',
      tab: 'Where next',
      content: (
        <>
          <p>
            The encoder measures the shaft faithfully and has no idea where its own zero sits
            relative to the magnets. Closing that gap is what <strong>Commutation Offset</strong>{' '}
            covers.
          </p>
          <p>
            <Link
              to="/learn/commutation-offset"
              className="text-syn-red hover:text-ocean transition-colors"
            >
              Commutation Offset →
            </Link>
          </p>
        </>
      ),
    },
  ]

  return (
    <div>
      <PageHeader
        eyebrow="Learn"
        title="Encoders"
        description="What the feedback on a motor shaft reports, how the kinds differ, and how finely any of it can be trusted."
      />
      <LearnSections sections={sections} />
    </div>
  )
}
