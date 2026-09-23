import { useState } from 'react'
import { Link } from 'react-router'
import PageHeader from '../components/PageHeader'
import LearnSections from '../components/LearnSections'
import type { LearnSectionEntry } from '../components/LearnSections'
import CommutationOffsetLab from '../components/CommutationOffsetLab'
import Callout from '../components/Callout'

// Learn → Commutation Offset.
//
// The concept first and the vendor second: every section but the last describes any three-phase
// permanent-magnet motor with any encoder on it, and only the last names an object index.
//
// The machine itself is described on Servo Motor Basics rather than here. This page opens with the
// three facts it borrows, each linking back, so it can be read straight through without leaving —
// a prerequisite the reader has to go and fetch is a page nobody finishes.

export default function LearnCommutationOffsetPage() {
  // The lab keeps its own pole pair control, so this page works without the cross-section beside
  // it.
  const [polePairs, setPolePairs] = useState(1)

  const sections: LearnSectionEntry[] = [
    {
      id: 'the-offset',
      title: 'What the offset is',
      tab: 'The offset',
      content: (
        <>
          <p>
            Three facts from{' '}
            <Link to="/learn/servo-motors" className="text-syn-red hover:text-ocean transition-colors">
              Servo Motors
            </Link>{' '}
            and{' '}
            <Link to="/learn/encoders" className="text-syn-red hover:text-ocean transition-colors">
              Encoders
            </Link>{' '}
            carry this page. Each is a link if you want the full version, but the one-line form is
            enough to read on.
          </p>
          <ul className="max-w-3xl space-y-2 list-disc pl-5 marker:text-syn-red">
            <li>
              The encoder measures the shaft faithfully, but where its own zero sits relative to the
              magnets was settled by assembly and means nothing to the drive.{' '}
              <Link to="/learn/encoders#where-it-sits" className="text-syn-red hover:text-ocean transition-colors">
                Where the encoder sits
              </Link>
            </li>
            <li>
              Commutation happens in electrical angle, which is shaft angle multiplied by the pole
              pair count.{' '}
              <Link to="/learn/servo-motors#shaft-and-electrical-angle" className="text-syn-red hover:text-ocean transition-colors">
                Two angles
              </Link>
            </li>
            <li>
              An encoder reports counts, not degrees. Degrees are the drive&apos;s arithmetic.{' '}
              <Link to="/learn/encoders#counts-and-resolution" className="text-syn-red hover:text-ocean transition-colors">
                What the encoder reports
              </Link>
            </li>
          </ul>
          <p>
            So the encoder&apos;s angle is right in its steps and wrong in its starting point. The
            gap is a fixed number for that one assembly, and it is called the{' '}
            <strong>commutation offset</strong>. Nothing the drive already knows lets it work the
            number out. It has to be measured once and stored.
          </p>
          <p>
            That is what the whole procedure is for, and the goal is worth stating plainly before
            any of the mechanism. The drive has to switch current into the phases at the right
            moment, and the offset is what tells it when that moment is. Get it right and every amp
            the drive sends is making torque. Get it wrong and some of that current makes none.
          </p>
          <p>
            It is not that a wrong offset makes heat and a right one does not. Current heats the
            windings either way, because the winding has resistance and I²R does not care which
            direction the current points. What the offset
            decides is how much torque you get per amp. With the wrong number the drive has to send
            more current to reach the same torque, and it pays for the extra in heat it gets nothing
            back for.
          </p>
        </>
      ),
    },
    {
      id: 'try-it',
      title: 'Try it',
      content: (
        <>
          <p>
            Two dials, one machine, the same instant. <strong>The machine</strong> shows the parts
            where they physically sit. <strong>What the controller sees</strong> shows that same
            moment in electrical angle, which is the only place commutation happens.
          </p>
          <CommutationOffsetLab polePairs={polePairs} onPolePairsChange={setPolePairs} />
          <p>
            The yellow band on the machine dial is the gap between the encoder&apos;s zero mark and
            the magnets. Turn the shaft and the band travels with the rotor, because it belongs to
            the assembly and not to any one position. Drag the outer ring instead and the band
            changes, which is what re-mounting the encoder would do.
          </p>
          <p>
            Then move the stored offset away from the correct value and watch the controller dial.
            The drive keeps pointing its current a quarter turn ahead of where it{' '}
            <em>believes</em> the magnets are, so the further its belief drifts, the less of that
            current makes torque. <strong>Measure the offset</strong> puts the right number in.
          </p>
          <p>
            The two readouts to watch are the band, measured in degrees of shaft rotation, and the
            stored offset, in electrical degrees. At one pole pair they are the same number. Raise
            the pole pairs and they part company.
          </p>
        </>
      ),
    },
    {
      id: 'cost-of-a-wrong-offset',
      title: 'What a wrong offset costs',
      tab: 'What it costs',
      content: (
        <>
          <p>
            Torque comes from current at right angles to the rotor&apos;s magnetic axis. Current
            pointing straight at the magnets only pulls them outward against the bearings, which
            turns nothing. So the drive points its current a quarter turn ahead of where it
            believes the magnets are: 90 electrical degrees. Field-oriented control names the two
            directions — the magnet axis is the <strong>d axis</strong>, the torque-producing axis
            90 degrees on is the <strong>q axis</strong>.
          </p>
          <p>
            Which side the current sits on is what sets the direction of travel. A quarter turn
            ahead pulls the rotor forward, a quarter turn behind pulls it backward. That is how a
            drive reverses: it does not rewire anything and it does not change how much current it
            sends, it puts that current on the other side of the rotor.
          </p>
          <p>
            If the belief is wrong by an angle, so is the current. Split it into the two axes and
            the whole cost falls out of one line:
          </p>
          <p className="font-mono text-xs bg-grey-50 border border-grey-200 px-4 py-3 leading-5">
            q-axis current, makes torque ∝ cos(error)
            <br />
            d-axis current, makes none ∝ sin(error)
          </p>
          <p>
            The numbers that follow are worth knowing. At 30 degrees of error the motor still makes
            87 percent of its torque. At 60 degrees, half. At 90 degrees it makes none at all while
            drawing full current.
          </p>
          <p>
            Past 90 degrees the sign flips, and the reason is the one above: the position the drive
            calculated as a quarter turn ahead is really behind the magnets, so the pull is
            backward. An axis commanded to hold still then pushes away from the target, measures a
            larger error, and pushes harder. That is the runaway case, and it is why an offset can
            be wrong in a way that is not merely inefficient.
          </p>
          <p>
            The small error is the dangerous one. A badly commissioned axis with 20 or 30 degrees of
            error still moves, still holds position, and passes a quick test. It draws more current
            than it should for the torque it makes, and the difference goes into heating the
            windings. The <strong>Current making no torque</strong> bar in the lab above is that
            share.
          </p>
        </>
      ),
    },
    {
      id: 'where-zero-comes-from',
      title: 'Where zero comes from',
      tab: 'Where zero is',
      content: (
        <>
          <p>
            Before anything can be measured there has to be something to measure against, and what
            that is turns out not to be obvious. The magnets carry no marking. There is nothing on the rotor to measure from. Electrical zero is a{' '}
            <strong>convention</strong>, and it lives on the stator: it is the position where the
            rotor&apos;s north lines up with a chosen reference direction in the windings,
            conventionally the magnetic axis of phase U.
          </p>
          <p>
            It has to be that way round. The stator is the half the drive controls. It decides the
            currents, so it knows exactly which way the field it makes is pointing. The rotor is the
            unknown. Only the known half can carry the reference.
          </p>
          <p>
            The drive does not find that reference inside the motor, and it has no idea which slot
            is which. It has three wires. It <em>defines</em> the reference by what it does at them:
            push current in at U and let it return through V and W, and the field points one
            particular way. That direction is electrical zero. Where it lands inside the machine
            depends on the winding and the wiring, and the drive never needs to know.
          </p>
          <p>
            Which reference a given firmware picks is its own decision. It does not matter to you,
            as long as the same convention is used to measure the offset and to drive the motor
            afterwards.
          </p>
          <p>
            That is also why the order of the three motor wires matters. Swap two of them and the
            same current pattern makes a different field direction, with the sequence running the
            other way. The reference has moved and nothing told the drive. Establishing the phase
            order therefore has to come before measuring the offset, not after it.
          </p>
        </>
      ),
    },
    {
      id: 'how-it-is-measured',
      title: 'How the offset is measured',
      tab: 'How it is measured',
      content: (
        <>
          <p>
            No drive can work the number out from what it already knows, so every drive finds it by
            experiment. The most direct method drives current into the windings at an electrical
            angle the drive chooses. The rotor swings into line with that field the way a compass needle
            lines up with a magnet. The drive then reads the encoder and knows, for that one moment,
            what the encoder says when the magnets are at a known angle. The offset follows.
          </p>
          <p>
            Notice what is actually measured. Not the offset — there is no sensor for it. One
            encoder reading, taken at an instant when the rotor&apos;s electrical angle is already
            known because the drive put it there. Everything else is arithmetic:
          </p>
          <p className="font-mono text-xs bg-grey-50 border border-grey-200 px-4 py-3">
            offset = chosen field angle − encoder reading × pole pairs
          </p>
          <p>
            Worked through, with 2 pole pairs and an encoder that happens to have been fitted 110
            degrees of shaft away from where the drive&apos;s electrical zero lands. Nobody knows
            that 110 — it is what the procedure is about to find.
          </p>
          <div className="border border-grey-200 overflow-x-auto max-w-3xl">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  {['Step', 'Value', 'Known or measured'].map(h => (
                    <th
                      key={h}
                      className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap"
                    >
                      {h}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody className="text-grey-700">
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2">Drive picks a field direction</td>
                  <td className="px-4 py-2 font-mono">0° electrical</td>
                  <td className="px-4 py-2">Chosen. It energises U, V, W to point the field there.</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2">Rotor swings into line</td>
                  <td className="px-4 py-2 font-mono">shaft settles at 180°</td>
                  <td className="px-4 py-2">Neither. Nothing measures this.</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2">Drive reads the encoder</td>
                  <td className="px-4 py-2 font-mono">70° of shaft</td>
                  <td className="px-4 py-2">Measured. The only measured number here.</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2">Convert to electrical</td>
                  <td className="px-4 py-2 font-mono">70 × 2 = 140°</td>
                  <td className="px-4 py-2">Arithmetic</td>
                </tr>
                <tr>
                  <td className="px-4 py-2">Subtract</td>
                  <td className="px-4 py-2 font-mono">0 − 140 → 220°</td>
                  <td className="px-4 py-2">The offset. Stored.</td>
                </tr>
              </tbody>
            </table>
          </div>
          <p>
            Check it at a shaft position nobody visited during the procedure. Turn the shaft to 35
            degrees and the encoder reads 285. The drive computes{' '}
            <span className="font-mono">285 × 2 + 220 = 790</span>, which wraps to 70. The true
            electrical angle is <span className="font-mono">35 × 2 = 70</span>. They agree, and that
            agreement is what the stored number buys.
          </p>
          <p>
            One detail that looks like a problem and is not. At 2 pole pairs, electrical zero
            happens at two shaft positions, 0 and 180 degrees, and the rotor may settle at either.
            Work the arithmetic through the other one and the offset still comes out 220, because
            the answer lives in electrical degrees where both positions are the same place.
          </p>
          <p>
            There are two families of method, and they do not resemble each other. The one above
            moves the rotor and reads the encoder, so it needs the shaft free and whatever it drives
            safe to move. The other family never moves the rotor at all: it probes the iron.
          </p>
          <p>
            Probing works because a magnet saturates the steel around it. The drive picks a trial
            angle, pushes a large current along it to drive the iron into saturation, injects a
            short voltage pulse and measures how much current that pulse produced. Saturated iron
            has less inductance, so the same pulse makes a bigger current step. Sweep the trial
            angle and the largest step marks the magnet direction.
          </p>
          <p>
            So the no-movement method does not ask less of the machine. It asks for a lot of
            current, held still, and it trades precision for the fact that nothing turns.
          </p>
          <p>
            How often it has to be repeated depends on which kind of encoder is fitted. An
            absolute encoder holds its meaning across a power cycle, so the offset is measured once
            at commissioning and stored. An incremental one starts from nothing at every power-up,
            so the relationship has to be established again each time the drive comes up.
          </p>
          <p>
            Either way the measurement holds only while the encoder stays where it was. Loosen it,
            replace it, change its resolution or its counting direction, and the number measured
            before is no longer about the machine in front of you.
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
            The stored offset is object <span className="font-mono">0x2001</span>. It is written by
            OS command 5, which the console runs as the{' '}
            <strong>Commutation offset measurement</strong> procedure under Devices → Procedures.
          </p>
          <p>
            Its unit is not degrees. The firmware maps one full electrical turn onto{' '}
            <span className="font-mono">0</span> to <span className="font-mono">4095</span>, so a
            count there is a 4096th of an electrical revolution and one electrical degree is about
            11 counts.
          </p>
          <p>
            Which method runs is a drive setting, <span className="font-mono">0x2009:03</span>. It
            changes what the command physically does, not only how precise it is:
          </p>
          <div className="border border-grey-200 overflow-x-auto max-w-3xl">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  {['0x2009:03', 'Method', 'Movement', 'Brake', 'Needs'].map(h => (
                    <th
                      key={h}
                      className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap"
                    >
                      {h}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody className="text-grey-700">
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono">0</td>
                  <td className="px-4 py-2">Constant-rate magnetic alignment</td>
                  <td className="px-4 py-2">Up to one pole pair</td>
                  <td className="px-4 py-2">Released</td>
                  <td className="px-4 py-2">Nothing. The default.</td>
                </tr>
                <tr className="border-b border-grey-100">
                  <td className="px-4 py-2 font-mono">1</td>
                  <td className="px-4 py-2">PID-based magnetic alignment</td>
                  <td className="px-4 py-2">Little to none if well tuned</td>
                  <td className="px-4 py-2">Released</td>
                  <td className="px-4 py-2">
                    Tuned gains in <span className="font-mono">0x2009:04-06</span>
                  </td>
                </tr>
                <tr>
                  <td className="px-4 py-2 font-mono">2</td>
                  <td className="px-4 py-2">High-frequency signal injection</td>
                  <td className="px-4 py-2">None</td>
                  <td className="px-4 py-2">Engaged</td>
                  <td className="px-4 py-2">Nothing. Less precise.</td>
                </tr>
              </tbody>
            </table>
          </div>
          <p>
            The names are the firmware&apos;s own, and they say what each one does.{' '}
            <strong>Method 0</strong> holds current on the rotor&apos;s magnet axis and sweeps that
            direction round at a steady rate, slowly enough for the rotor to follow. It settles when
            the rotor has tracked to within three electrical degrees and stayed there for half a
            second, and reports the average over that window.
          </p>
          <p>
            <strong>Method 1</strong> closes a loop instead of sweeping. A PID controller using the
            gains in <span className="font-mono">0x2009:04-06</span> steers the field to stay locked
            on the rotor, and the controller&apos;s own output is the offset. It settles in a tenth
            of a second, which is why a well tuned axis barely moves. With all three gains left at
            zero the loop is never closed and the method cannot work.
          </p>
          <p>
            <strong>Method 2</strong> is the probing method described above. It scans the electrical
            circle in ten-degree steps, then rescans the winning ten degrees one degree at a time.
            It needs no tuning and turns nothing, and it is the least precise of the three.
          </p>
          <p>
            The brake column is a real difference rather than a detail. The firmware refuses methods
            0 and 1 outright while the brake is engaged, unless the brake is set to manual release.
            Method 2 needs nothing of the brake, so it stays as it was and the console never touches
            it.
          </p>
          <Callout variant="warning" className="max-w-3xl">
            Motor phase order detection (OS command 4) must run first. An offset measured before the
            phase order is known is simply wrong, and nothing detects it afterwards, because{' '}
            <span className="font-mono">0x2003:05</span> holds a valid value either way. The{' '}
            <strong>Offset detection</strong> procedure runs the whole commissioning sequence in the
            right order, which is the safer way to get there.
          </Callout>
          <p>
            A successful run also sets <span className="font-mono">0x2009:01</span> to OFFSET_VALID.
            Both values land in the object dictionary and not in flash, so they are gone at the next
            power cycle unless something stores them. The drive marks the offset invalid on its own
            whenever the commutation encoder&apos;s source, type, resolution, polarity, singleturn
            offset or feedback type changes, or the pole pair count does.
          </p>
        </>
      ),
    },
  ]

  return (
    <div>
      <PageHeader
        eyebrow="Learn"
        title="Commutation Offset"
        description="The angle between the encoder's zero and the rotor's magnets, and why the drive has to be told it."
      />
      <LearnSections sections={sections} />
    </div>
  )
}
