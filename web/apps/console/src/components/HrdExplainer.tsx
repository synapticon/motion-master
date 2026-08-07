import Explainer from './Explainer'

// The "What is HRD?" teaching panel for the device HRD page. Its job is to answer the two questions
// this page cannot answer by itself: where a recording comes from (the procedure, not this page),
// and what the numbers on it actually are.
export default function HrdExplainer() {
  return (
    <Explainer title="What is HRD?">
      <p>
        <strong>HRD</strong> stands for <strong>high resolution data</strong>: a recording the drive
        makes <em>by itself</em>, into files on its own flash, and hands back afterwards over FoE.
        That is what makes it different from Monitoring and the Data Recorder, which sample from the
        master's side and are limited by what the process image carries and how often the loop runs.
        A signal the drive knows but never puts on the wire can still be recorded here.
      </p>
      <p>
        <strong>One sample per millisecond.</strong> The drive's control loop runs faster — as often
        as once every 250 µs — but the recording is decimated to 1 kHz whatever the loop period is,
        so a recording's length is not the drive's resolution. The samples are evenly spaced, which
        is why the plot's x axis is a sample number: on current firmware sample <em>n</em> is also
        millisecond <em>n</em>.
      </p>
      <p>
        <strong>Recording happens on the Procedures page</strong>, as <em>HRD streaming</em>. This
        page only reads back what is already on the drive. That split is deliberate: a recording is
        worth reading more than once, and it survives until the next recording overwrites it — the
        arming step deletes the previous files, so read one back before making the next.
      </p>
      <p>
        <strong>Two kinds of recording, and you have to say which.</strong> Nothing on the drive
        records what its files hold, so reading takes the same choice the recording was made with;
        pick the wrong one and the same bytes decode as a different layout, producing numbers that
        look plausible and are not.
      </p>
      <ul className="list-disc pl-5 space-y-1">
        <li>
          <strong>Encoder raw data</strong> — the raw position word an iC-MU encoder reports.{' '}
          <code>raw</code> is the 32-bit word as recorded; <code>masterCount</code> and{' '}
          <code>noniusCount</code> are the two 14-bit tracks packed inside it (bits 0-13 and 14-27),
          which are what an encoder calibration works from. It records zeros unless the encoder was
          put into raw mode first with the <em>iC-MU calibration mode</em> procedure.
        </li>
        <li>
          <strong>System identification data</strong> — <code>velocityRpm</code>, converted here out
          of the drive's Q15 fixed point into real RPM, and <code>torquePermil</code>, in per mille
          of rated torque. It records an unexcited drive unless a system identification run was
          started first.
        </li>
      </ul>
      <p>
        <strong>Why the byte counts do not divide evenly.</strong> The drive writes into at most
        five 8032-byte files as one continuous stream, so a sample can straddle a file boundary and
        the last block is padded. Those padding bytes are reported as <em>trailing</em> rather than
        decoded into an invented final sample.
      </p>
    </Explainer>
  )
}
