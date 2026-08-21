import Explainer from './Explainer'

// The "What is auto-tuning?" teaching panel for the Tools page. Explains the separate process, why
// it is separate, and what the two heavier functions do — a reader who has never tuned a drive
// should be able to tell the operations apart before choosing one.
export default function AutoTuningExplainer() {
  return (
    <Explainer title="What is auto-tuning?">
      <p>
        <strong>Auto-tuning</strong> computes controller gains for a drive from a{' '}
        <strong>plant model</strong>: a transfer function, a numerator and a denominator, describing
        how the machine responds to torque. Every tuning function here starts from one.
      </p>
      <p>
        A plant model comes from a measurement, and the measurement is made elsewhere. The{' '}
        <strong>System Identification</strong> procedure runs on the drive: it excites the machine
        with a <strong>chirp</strong> — a torque signal that sweeps a frequency range — and records
        the response. This program has no part in that and reaches no device. What it does is the
        step after: <code>identify_plant_model</code> fits a plant model to the samples that
        recording produced.
      </p>
      <p>
        The functions run in a <strong>separate program</strong>, which Motion Master starts as a
        child process and calls over the loopback interface. It holds the algorithms, it is compiled
        from its own source, and it is about 65 MB — far larger than the server itself. That is why
        it is downloaded once by the install script rather than shipped in every release, and why a
        machine may not have it. Nothing else on this server depends on it.
      </p>
      <p>
        It is <strong>a commissioning tool</strong>. Gains are computed while a drive is being
        configured; a machine that only runs an already-configured drive never calls it. So an
        installation can leave it out, and this page then says so instead of offering controls that
        cannot work.
      </p>
      <p>
        Measurement data travels <strong>in the request body</strong>. The chirp measurements are
        uploaded as CSV text and the resulting bode data comes back inline, so nothing writes a file
        and the two processes need no shared view of a filesystem.
      </p>
      <p>
        <strong>A rejected input answers with 200.</strong> That program reports a refusal — missing
        inputs, a model that cannot be fitted — as an <code>error</code> property in an otherwise
        normal reply, and keeps error statuses for a malformed request, an unknown function name and
        a routine that threw. This page shows both as a failure; a client reading the API should test
        for <code>error</code> rather than trusting the status alone.
      </p>
      <p>
        <strong>Which function to use.</strong> The <code>auto_tune_*</code> functions compute gains
        for one loop from a plant model and a target, and are the direct calculation. The{' '}
        <code>full_auto_tune_*</code> functions take the drive's own configuration as well — filters,
        notch, current-loop gains, DC link voltage, rated torque — and return stability margins
        alongside the gains, which is what a commissioning tool wants.{' '}
        <code>identify_plant_model</code> is the step before either: it turns chirp measurements into
        the model they need. <code>auto_tune_notch</code> and{' '}
        <code>auto_tune_feedback_filters</code> tune a single feature rather than a loop.
      </p>
      <p>
        Nothing on this page moves a motor. Every call here computes on the numbers in the request.
        The chirp that produces a measurement is the System Identification procedure, on the device.
      </p>
    </Explainer>
  )
}
