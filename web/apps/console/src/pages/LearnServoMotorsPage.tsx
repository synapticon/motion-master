import { useState } from 'react'
import PageHeader from '../components/PageHeader'
import LearnSections from '../components/LearnSections'
import type { LearnSectionEntry } from '../components/LearnSections'
import MotorCrossSection from '../components/MotorCrossSection'
import CoilPairField from '../components/CoilPairField'
import StatorFieldFigure from '../components/StatorFieldFigure'
import TorqueSpeedFigure from '../components/TorqueSpeedFigure'
import Term from '../components/Term'

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
      id: 'what-is-a-servo',
      title: 'What a servo is',
      tab: 'Servo',
      content: (
        <>
          <p>
            A plain DC or AC motor turns while it is powered. Nothing in it knows where the shaft is,
            so on its own it cannot be told to stop in a particular place. That is fine for a fan or
            a pump, where all you want is rotation. It rules out anything that has to arrive
            somewhere: a robot joint, a machine axis, a gripper.
          </p>
          <p>
            A servo is three parts that work together: the motor, an{' '}
            <Term id="encoder">encoder</Term> on its shaft that reports where the shaft is, and a{' '}
            <strong>drive</strong> that powers the motor. The drive compares the position the
            encoder reports against the commanded one and corrects the difference, the{' '}
            <Term id="following-error">following error</Term>. Command, measure, correct: that is
            what <strong>servo</strong> means.
          </p>
          <p>
            What you command can be a position, a velocity or a torque. All three arrive at the
            same instruction in the end: how much current to push through the motor.
          </p>
          <p>
            The drive does that conversion itself, in firmware. A position or velocity command
            becomes a torque demand, and a torque demand becomes current in the motor. Each step is
            a control loop. The last one, the torque loop, turns the torque demand into current, and
            it runs the fastest.
          </p>
        </>
      ),
    },
    {
      id: 'what-is-inside',
      title: 'What is inside',
      tab: 'Inside',
      content: (
        <>
          <p>
            Inside a servo motor there are two parts. The <Term id="rotor">rotor</Term> is the
            turning part: the shaft and the permanent magnets fixed to it. The{' '}
            <Term id="stator">stator</Term> is the stationary part around it, and it carries the
            coils, wired into three groups called <Term id="phase">phases</Term>.
          </p>
          <p>
            Current in the phases makes a magnetic field. That field attracts the rotor&apos;s
            opposite poles and pushes its like poles away. Both forces turn the rotor the same way,
            until its poles line up with the field. The drive sets the current, so the drive decides
            where the field points.
          </p>
          <p>
            A phase is one wire. It runs down the motor through one <Term id="slot">slot</Term> in
            the stator and comes back through another, so its two slots carry current opposite ways.
            That sounds like it should cancel. It does the opposite.
          </p>
          <CoilPairField />
          <p>
            So a loop that carries current is a magnet. Its field goes through the loop in one
            direction. The face where the field comes out is its north pole, and the face where the
            field goes back in is its south pole. Reverse the current and the two poles swap. A
            permanent magnet has fixed poles, but a coil&apos;s poles are wherever the current puts
            them. That is how the drive moves the stator&apos;s poles round without moving anything.
          </p>
          <MotorCrossSection polePairs={polePairs} onPolePairsChange={setPolePairs} />
          <p>
            Many servo motors carry a <Term id="holding-brake">holding brake</Term>. It is a
            friction disc held open by a coil, so it grips whenever the coil is unpowered. That is
            deliberate: a vertical axis should not drop when the power fails, and a brake that
            needed power to hold would do exactly that.
          </p>
          <p>
            It is a <em>holding</em> brake, not a stopping one. It keeps a stationary axis where it
            is, rather than bringing a moving one to rest. That matters during commissioning,
            because the brake is engaged by default and any procedure that turns the rotor has to
            release it first. Whatever the brake was holding is then free to move.
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
            Start with a brushed motor, which does the switching mechanically. Its magnets are
            fixed to the housing and the windings spin. Two brushes press on
            a <Term id="commutator">commutator</Term>, a ring of segments on the shaft, and as the
            shaft turns the segments pass under the brushes and switch current from one winding to
            the next, so the rotor&apos;s field never lines up with the magnets. Fields that line up
            make no torque, so keeping them apart is the whole job. Nothing has to measure anything,
            because the part doing the switching turns with the rotor.
          </p>
          <p>
            A servo motor is that turned inside out. The magnets spin and the windings sit still
            against the housing, where their heat can escape and nothing rubs. Nothing slides, so
            there are no brushes to wear out. But the commutator went with them, and the switching
            it was doing still has to happen.
          </p>
          <p>
            That switching has a name. <Term id="commutation">Commutation</Term> is moving the
            current from winding to winding as the rotor turns, so the fields never line up and the
            torque keeps coming. A brushed motor does it with the commutator. A servo drive has to
            do it itself.
          </p>
          <p>
            Doing it itself means the drive sets the three currents, so it can point the field
            wherever it likes. But torque depends on where the field sits relative to the magnets,
            and nothing can be placed relative to a thing whose position it does not know. So every
            switching decision, thousands per second, needs the same input: where the rotor is
            pointing right now.
          </p>
          <p>
            Here is the switching itself. The three coils never move, and there is no moment when
            the drive picks one of them. It sets all three currents at once, and their sum is a
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
            The in-between comes from switching fast. The transistors switch on and off tens of
            thousands of times a second, and the winding&apos;s{' '}
            <Term id="inductance">inductance</Term> smooths that chopped voltage into a current that
            follows the average. Spend more of each switching cycle on the positive rail and the
            average rises. Vary that share along a sine and the current follows a sine.
          </p>
          <p>
            So the drive builds three-phase AC out of a DC supply by deciding, tens of thousands of
            times a second, which rail each phase is connected to. That switching is{' '}
            <Term id="pwm">PWM</Term>, pulse-width modulation, and a power stage that does it is
            what makes a servo drive an <Term id="inverter">inverter</Term>. The voltage on a phase
            is never a sine wave. It is a train of full-height pulses whose average is one.
          </p>
          <p>
            The drive also measures the current that actually results and adjusts the pulse widths
            to close the gap. So when this page says a drive commands current, it means it
            literally: there is a loop doing that, thousands of times a second, on top of the
            switching.
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
            repeating pattern of magnets and <Term id="slot">slots</Term>. One full cycle of that
            pattern is 360 electrical degrees.
          </p>
          <p>
            The <Term id="pole-pair">pole pair</Term> count is where the difference comes from. With
            one pole pair the pattern fills the circle exactly once, so the two angles are the same
            number. With two it fills the circle twice, so one turn of the shaft is two complete
            electrical cycles. With four, four cycles.
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
            <a href="#what-is-inside" className="text-syn-red hover:text-ocean transition-colors">
              cross-section further up
            </a>{' '}
            shows both numbers as you drag its slider: the electrical angle the drive sets, and the
            shaft angle it produces. Change the pole pair count and watch the second one shrink.
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
            motor turns current into torque at a fixed rate called the{' '}
            <Term id="torque-constant">torque constant</Term>, quoted in newton metres per amp.
            Double the current and you double the torque, until the iron reaches{' '}
            <Term id="saturation">saturation</Term> or something gets too hot.
          </p>
          <p>
            That is only true of the current pointed the right way. Current aimed straight at the
            magnets makes no torque at all, however much of it there is, which is the same rule as
            before: fields that line up make nothing. Getting it pointed the right way is what the
            commutation offset is about.
          </p>
          <p>
            Velocity works the other way round. Turn the motor and it generates voltage, its{' '}
            <Term id="back-emf">back-EMF</Term>, rising in proportion to velocity. A drive can only
            push current into the motor while its supply voltage is higher than that, so the supply
            sets a ceiling on velocity. Approaching the ceiling there is less voltage left over to
            force current through, which is why the torque a motor can produce falls away as it runs
            faster.
          </p>
          <p>
            There is a way past that ceiling, and a drive will offer it as a setting.{' '}
            <Term id="field-weakening">Field weakening</Term> spends part of the current on a field
            that opposes the magnets instead of on making torque. Weaker magnets mean less back-EMF,
            which leaves room for more velocity. Nothing is free: the current doing the weakening is
            not making torque, so the motor turns faster and makes less of it.
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
          <p>A drive knows nothing about the motor bolted to it until someone fills these in.</p>
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
            pole pair count and whether the phases are inverted all have commissioning procedures
            that find them, on Devices → Procedures. The torque constant and the two rated figures
            come off the motor datasheet.
          </p>
          <p>
            <strong>Torque is not sent in newton metres.</strong> Target torque{' '}
            <span className="font-mono">0x6071</span>, torque demand{' '}
            <span className="font-mono">0x6074</span> and torque actual value{' '}
            <span className="font-mono">0x6077</span> are all fractions of the motor&apos;s rated
            torque, counted in thousandths. Write <span className="font-mono">1000</span> and you
            are asking for the full rated torque set in <span className="font-mono">0x6076</span>.
          </p>
          <p className="font-mono text-xs bg-grey-50 border border-grey-200 px-4 py-3">
            torque in mNm = value / 1000 × 0x6076
          </p>
          <p>
            So with <span className="font-mono">0x6076</span> set to 400 mNm, a target of{' '}
            <span className="font-mono">250</span> asks for 100 mNm, and{' '}
            <span className="font-mono">-1000</span> asks for full rated torque the other way.{' '}
            <span className="font-mono">0x6071</span> is signed, which is how direction is
            expressed. What stops it going too far is the limits below, not the size of the field.
          </p>
          <p>
            Two reasons it is done this way. The number stays meaningful when the motor changes, and
            it fits in sixteen bits, which matters when it is sent to the drive every cycle. The
            trap is the other side of that: change <span className="font-mono">0x6076</span> and
            every torque number on the bus means something different, while none of them appear to
            have changed.
          </p>
          <p>
            <strong>How a position command reaches the torque demand is a choice.</strong>{' '}
            <span className="font-mono">0x2002</span> selects it. Simple PID puts one position
            controller straight onto the torque demand. Cascaded PID has the position controller
            feed a velocity controller first, and that one produces the torque demand. The position
            and velocity loops run at 4 kHz, the torque loop at 16 kHz.
          </p>
          <p>
            <strong>Field weakening</strong> is off by default and lives in the torque controller
            record. <span className="font-mono">0x2010:04</span> enables it and{' '}
            <span className="font-mono">0x2010:05</span> sets how much current is spent on the
            weakening, as a percentage of the rated current. The dictionary warns that more than 25
            percent can destabilise the torque controller, and how much is too much depends on the
            motor. There are also starting and
            ending speed sub-entries at <span className="font-mono">0x2010:06</span> and{' '}
            <span className="font-mono">0x2010:07</span>, both marked deprecated, so do not build
            anything on them.
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
                  <td className="px-4 py-2 font-mono">thousandths of 0x6076</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono whitespace-nowrap">0x6073</td>
                  <td className="px-4 py-2">Max current</td>
                  <td className="px-4 py-2 font-mono">thousandths of 0x6075</td>
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
            you intend to run the machine, and the drive stays inside them in normal use.{' '}
            <span className="font-mono">0x6073</span> is where the peak current from the section
            above is set, as a multiple of the rated current in{' '}
            <span className="font-mono">0x6075</span>.
          </p>
          <p>
            The <span className="font-mono">0x2006</span> protection setpoints are for abnormal
            conditions and are normally set wider, because they exist to catch a fault rather than
            to shape everyday behaviour. Setting a protection limit down at the operating limit
            turns every ordinary peak into a fault.
          </p>
          <p>
            Separately, <span className="font-mono">0x200A</span> holds I²t and stall protection.
            That is about how long a current is drawn rather than how large it is. It catches a
            motor quietly cooking at a current no instantaneous limit would object to.
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
            Every one of those switching decisions needs the rotor angle, and the encoder named at
            the top of this page is the only thing that supplies it. There is more to it than one
            number: how finely an encoder reads, what it knows the moment the power comes on, and
            what it costs you when it only counts.
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
        description="What servo means, how a servo motor is built and commutated, how current becomes torque, and how a SOMANET drive is set up for its motor."
      />
      <LearnSections sections={sections} />
    </div>
  )
}
