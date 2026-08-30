import { useState } from 'react'
import { Link } from 'react-router'
import PageHeader from '../components/PageHeader'
import LearnSections from '../components/LearnSections'
import type { LearnSectionEntry } from '../components/LearnSections'
import MotorCrossSection from '../components/MotorCrossSection'
import CoilPairField from '../components/CoilPairField'
import TorqueSpeedFigure from '../components/TorqueSpeedFigure'
import StatorFieldFigure from '../components/StatorFieldFigure'

// Learn → Servo Motors.
//
// The machine itself, and nothing else: construction, commutation, the two angles, and how current
// becomes torque. Feedback has its own page because it grew to be the larger topic, and because
// every other Learn page needs it independently of this one.
//
// Section ids are linked to from other pages, so treat them as stable.

export default function LearnServoMotorsPage() {
  const [polePairs, setPolePairs] = useState(1)

  const sections: LearnSectionEntry[] = [
    {
      id: 'what-is-inside',
      title: 'What is inside',
      tab: 'Inside',
      content: (
        <>
          <p>
            The motor on the end of a servo drive is a three-phase permanent-magnet machine: a{' '}
            <strong>PMSM</strong> (permanent magnet synchronous motor), a <strong>BLDC</strong>{' '}
            (brushless DC motor), or a synchronous AC motor. The BLDC name says what is missing.
            There are no brushes inside, and the job the brushes used to do still has to be done by
            something.
          </p>
          <p>
            The two names describe the shape of the voltage the motor generates when you turn it by
            hand, its <strong>back-EMF</strong>: roughly trapezoidal for a BLDC, sinusoidal for a
            PMSM. That shapes how each is
            best driven, six-step switching or smooth field-oriented control. Everything on this
            page applies to both.
          </p>
          <p>
            Inside, the rotor carries magnets. Around it sit coils, wired into three groups called{' '}
            <strong>phases</strong>. Current in the phases makes a magnetic field, and the drive
            chooses which way that field points, keeping it ahead of the magnets so they are pulled
            round.
          </p>
          <p>
            That layout is a brushed motor turned inside out. A brushed motor keeps its magnets
            still and spins the current-carrying windings, which is exactly why it needs sliding
            contacts: something has to feed a winding that is moving. Swap the two and the windings
            end up against the housing, where their heat can escape and nothing has to rub. The cost
            of the swap is that the switching leaves the motor and becomes the drive&apos;s problem.
          </p>
          <MotorCrossSection polePairs={polePairs} onPolePairsChange={setPolePairs} />
          <CoilPairField />
          <p>
            Many servo motors carry a <strong>holding brake</strong>, and it is worth knowing about
            because so much of commissioning has to work around it. It is a friction disc held open
            by a coil, so it grips whenever the coil is unpowered. That is deliberate. A vertical axis or a robot joint
            should not drop when the power fails, and a brake that needs power to hold would do
            exactly that.
          </p>
          <p>
            It is a <em>holding</em> brake, not a stopping one. It is meant to keep a stationary axis
            where it is, not to bring a moving one to rest. The consequence for anyone commissioning
            a drive is that the brake is engaged by default, and a procedure that has to turn the
            rotor must release it first — which means whatever the brake was holding is then free to
            move.
          </p>
        </>
      ),
    },
    {
      id: 'commutation',
      title: 'Commutation',
      content: (
        <>
          <p>
            <strong>Commutation</strong> is the job the brushes used to do, and the word comes from
            the part these motors do not have. Those sliding contacts press on a{' '}
            <strong>commutator</strong>: a ring of segments built into the rotor. As the shaft
            turns, the segments pass under the brushes and switch current between the windings, so
            the magnetic pull always stays ahead of the rotor. The timing needs nothing to measure
            it, because the part doing the switching turns with the rotor.
          </p>
          <p>
            Take that away and the drive has to do the switching itself. It can point the field
            wherever it likes, since it decides the currents. But torque depends on where the field
            sits <em>relative to the magnets</em>, and nothing can be placed relative to something
            whose position it does not know. That is why every switching decision, thousands per
            second, needs the same input: where the rotor is pointing right now.
          </p>
          <p>
            Here is the switching itself. The three coils never move, and there is no moment when
            the drive picks one of them — it sets all three currents at once, and their sum is a
            field it can aim anywhere.
          </p>
          <StatorFieldFigure />
          <p>
            The figure shows those three currents as if the drive had a dial for each one. It does
            not. It has a fixed DC supply and six transistors, two per phase, and each pair can
            connect its phase either to the positive rail or to the negative one. There is no
            in-between setting.
          </p>
          <p>
            The in-between comes from switching fast. The transistors switch on and off tens of thousands of
            times a second, and the winding&apos;s inductance smooths that chopped voltage into a
            current that follows the average. Spend more of each switching cycle on the positive
            rail and the average rises. Vary that share along a sine and the current follows a sine.
          </p>
          <p>
            So the drive builds three-phase AC out of a DC supply by deciding, tens of thousands of
            times a second, which rail each phase is connected to. That switching is{' '}
            <strong>PWM</strong>, pulse-width modulation, and a power stage that does it is what
            makes a servo drive an <strong>inverter</strong>. The voltage on a phase is never a sine
            wave. It is a train of full-height pulses whose average is one.
          </p>
          <p>
            The drive also measures the current that actually results and adjusts the pulse widths to
            close the gap. So when <strong>Current and torque</strong> below says a drive commands
            current, it means it literally: there is a loop doing that, thousands of times a second,
            on top of the switching.
          </p>
        </>
      ),
    },
    {
      id: 'shaft-and-electrical-angle',
      title: 'Shaft angle and electrical angle',
      tab: 'Two angles',
      content: (
        <>
          <p>
            Two angles matter here and they are not the same one. <strong>Shaft angle</strong> is
            what a protractor would read: one turn of the shaft is 360 degrees.{' '}
            <strong>Electrical angle</strong> is how far the drive has advanced through the
            repeating pattern of magnets and slots. One full cycle of that pattern is 360 electrical
            degrees.
          </p>
          <p>
            The pole pair count is where the difference comes from. With one pole pair the pattern
            fills the circle exactly once, so the two angles are the same number. With two it fills
            the circle twice, so one turn of the shaft is two complete electrical cycles. With four,
            four cycles.
          </p>
          <p className="font-mono text-xs bg-grey-50 border border-grey-200 px-4 py-3">
            electrical angle = shaft angle × pole pairs
          </p>
          <p>
            Commutation runs entirely in electrical angle. Nothing the drive does to make torque is
            expressed in shaft degrees, which is why the pole pair count turns up in every
            calculation that touches the rotor&apos;s position.
          </p>
          <p>
            The{' '}
            <a
              href="#what-is-inside"
              className="text-syn-red hover:text-ocean transition-colors"
            >
              cross-section further up
            </a>{' '}
            has a pole pair control if you want to watch the pattern repeat.
          </p>
        </>
      ),
    },
    {
      id: 'current-and-torque',
      title: 'Current and torque',
      tab: 'Torque',
      content: (
        <>
          <p>
            A drive does not command torque directly. It commands <strong>current</strong>, and the
            motor turns current into torque at a fixed rate called the <strong>torque constant</strong>,
            quoted in newton metres per amp. Double the current and you double the torque, until the
            iron begins to saturate or something gets too hot.
          </p>
          <p>
            That is only true of the current pointed the right way. Current aimed straight at the
            magnets makes no torque at all, however much of it there is. Getting it pointed the
            right way is what{' '}
            <Link
              to="/learn/commutation-offset"
              className="text-syn-red hover:text-ocean transition-colors"
            >
              Commutation Offset
            </Link>{' '}
            is about.
          </p>
          <p>
            Speed works the other way round. Turn the motor and it generates voltage, the back-EMF
            named earlier, rising in proportion to speed. A drive can only push current into the
            motor while its supply voltage is higher than that, so the supply sets a ceiling on
            speed. Approaching the ceiling there is less voltage left over to force current through,
            which is why the torque a motor can produce falls away as it runs faster.
          </p>
          <p>
            There is a way past that ceiling, and a drive will offer it as a setting.{' '}
            <strong>Field weakening</strong> spends part of the current on a field that opposes the
            magnets instead of on making torque. Weaker magnets mean less back-EMF, which leaves
            room for more speed. Nothing is free: the current doing the weakening is not making
            torque, so the motor turns faster and pulls less.
          </p>
          <TorqueSpeedFigure />
          <p>
            Two current numbers appear on every motor datasheet and mean different things.{' '}
            <strong>Rated</strong> current is what the windings can carry all day without
            overheating. <strong>Peak</strong> current is what they will take for a few seconds. A
            drive is configured with both, because an axis that accelerates hard and then holds
            still needs each in turn.
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
            A drive knows nothing about the motor bolted to it until someone fills these in.
          </p>
          <div className="border border-grey-200 overflow-x-auto max-w-3xl">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  <th className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">Object</th>
                  <th className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">What</th>
                  <th className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">Unit</th>
                </tr>
              </thead>
              <tbody className="text-grey-700">
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x2003:01</td>
                  <td className="px-4 py-2">Pole pairs</td>
                  <td className="px-4 py-2 font-mono">—</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x2003:02</td>
                  <td className="px-4 py-2">Torque constant</td>
                  <td className="px-4 py-2 font-mono">µNm/A</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x2003:03</td>
                  <td className="px-4 py-2">Phase resistance</td>
                  <td className="px-4 py-2 font-mono">µΩ</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x2003:04</td>
                  <td className="px-4 py-2">Phase inductance</td>
                  <td className="px-4 py-2 font-mono">µH</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x2003:05</td>
                  <td className="px-4 py-2">Motor phases inverted</td>
                  <td className="px-4 py-2 font-mono">—</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x2003:06</td>
                  <td className="px-4 py-2">Difference of Ld and Lq inductance</td>
                  <td className="px-4 py-2 font-mono">µH</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x6075</td>
                  <td className="px-4 py-2">Motor rated current</td>
                  <td className="px-4 py-2 font-mono">mA</td>
                </tr>
                <tr className="border-b border-grey-100 last:border-0">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x6076</td>
                  <td className="px-4 py-2">Motor rated torque</td>
                  <td className="px-4 py-2 font-mono">mNm</td>
                </tr>
              </tbody>
            </table>
          </div>
          <p>
            Four of those are measured rather than typed in. Phase resistance, phase inductance, the
            pole pair count and the phase order all have commissioning procedures that find them, on
            Devices → Procedures. The torque constant and the two rated figures come off the motor
            datasheet.
          </p>
          <p>
            <strong>Torque is not sent in newton metres.</strong> Target torque{' '}
            <span className="font-mono">0x6071</span>, torque demand{' '}
            <span className="font-mono">0x6074</span> and torque actual value{' '}
            <span className="font-mono">0x6077</span> are all fractions of the motor&apos;s rated
            torque, counted in thousandths. Write{' '}
            <span className="font-mono">1000</span> and you are asking for the full rated torque set
            in <span className="font-mono">0x6076</span>.
          </p>
          <p className="font-mono text-xs bg-grey-50 border border-grey-200 px-4 py-3">
            torque in mNm = value / 1000 × 0x6076
          </p>
          <p>
            So with <span className="font-mono">0x6076</span> set to 400 mNm, a target of{' '}
            <span className="font-mono">250</span> asks for 100 mNm, and{' '}
            <span className="font-mono">-1000</span> asks for full rated torque the other way.{' '}
            <span className="font-mono">0x6071</span> is signed, which is how direction is expressed;
            what stops it going too far is the limits below, not the size of the field.
          </p>
          <p>
            Two reasons it is done this way. The number stays meaningful when the motor changes, and
            it fits in sixteen bits, which matters when it is going into a process image every
            cycle. The trap is the other side of that: change{' '}
            <span className="font-mono">0x6076</span> and every torque number on the bus means
            something different, while none of them appear to have changed.
          </p>
          <p>
            <strong>Field weakening</strong> is off by default and lives in the torque controller
            record. <span className="font-mono">0x2010:04</span> enables it and{' '}
            <span className="font-mono">0x2010:05</span> is how far the rotor field is reduced, as a
            percentage. There are also starting and ending speed sub-entries at{' '}
            <span className="font-mono">0x2010:06</span> and{' '}
            <span className="font-mono">0x2010:07</span>; the dictionary marks both deprecated, so do
            not build anything on them.
          </p>
          <p>
            <strong>There are two kinds of limit, and they are easy to confuse.</strong>
          </p>
          <div className="border border-grey-200 overflow-x-auto max-w-3xl">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  <th className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">Object</th>
                  <th className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">What</th>
                  <th className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">Unit</th>
                </tr>
              </thead>
              <tbody className="text-grey-700">
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x6072</td>
                  <td className="px-4 py-2">Max torque</td>
                  <td className="px-4 py-2 font-mono">per mille of 0x6076</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x6073</td>
                  <td className="px-4 py-2">Max current</td>
                  <td className="px-4 py-2 font-mono">per mille of 0x6075</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x6080</td>
                  <td className="px-4 py-2">Max motor speed</td>
                  <td className="px-4 py-2 font-mono">rpm</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x2006:01</td>
                  <td className="px-4 py-2">Undervoltage setpoint</td>
                  <td className="px-4 py-2 font-mono">V</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x2006:02</td>
                  <td className="px-4 py-2">Overvoltage setpoint</td>
                  <td className="px-4 py-2 font-mono">V</td>
                </tr>
                <tr className="border-b border-grey-100 last:border-0">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x2006:03</td>
                  <td className="px-4 py-2">Overcurrent setpoint</td>
                  <td className="px-4 py-2 font-mono">mA</td>
                </tr>
              </tbody>
            </table>
          </div>
          <p>
            <span className="font-mono">0x6072</span>, <span className="font-mono">0x6073</span> and{' '}
            <span className="font-mono">0x6080</span> are operating limits: they describe how hard
            you intend to run the machine, and the drive stays inside them in normal use. The{' '}
            <span className="font-mono">0x2006</span> protection setpoints are for abnormal
            conditions and are normally set wider, because they exist to catch a fault rather than to
            shape everyday behaviour. Setting a protection limit down at the operating limit turns
            every ordinary peak into a fault.
          </p>
          <p>
            Separately, <span className="font-mono">0x200A</span> holds I²t and stall protection,
            which is about how long a current is drawn rather than how large it is — the thing that
            catches a motor quietly cooking at a current no instantaneous limit would object to.
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
            The drive needs the rotor angle for every one of those switching decisions, and nothing
            in the motor itself supplies it. That is what the feedback on the shaft is for, and it
            has more to it than a single number.
          </p>
          <p>
            <Link to="/learn/encoders" className="text-syn-red hover:text-ocean transition-colors">
              Encoders →
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
        title="Servo Motors"
        description="How a permanent-magnet servo motor is built, what commutation is, and how the current a drive sends becomes torque."
      />
      <LearnSections sections={sections} />
    </div>
  )
}
