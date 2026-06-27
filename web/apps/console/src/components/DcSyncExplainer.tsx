import Explainer from './Explainer'

// The teaching panel for the Fieldbus → DC Sync page. It explains what distributed clocks are and
// why they matter, the reference/follower roles, the two register-backed columns (propagation delay
// 0x0928 and system-time difference 0x092C), why the difference only converges while the bus is
// exchanging, how to read "locked", and the important caveat that this driver measures/disciplines
// DC but does not activate SYNC0 (SM-synchronous bring-up) — consistent with the Configuration page.
export default function DcSyncExplainer() {
  return (
    <Explainer title="What is distributed-clock sync?">
      <p>
        <strong>Distributed Clocks (DC)</strong> give every slave on the bus a shared notion of time,
        accurate to tens of nanoseconds. Without it each slave would act whenever its frame happens to
        arrive, and because a frame reaches a slave at the far end of the chain later than one near the
        master, their actions would be skewed by the cable and forwarding delays between them. DC
        removes that skew, so multiple axes can act <em>in unison</em> — the foundation for
        coordinated multi-drive motion. This page shows how tightly each slave&apos;s clock is tracking
        the bus.
      </p>

      <p>
        The values are <strong>live</strong> and read on demand: each time the page asks, Motion Master
        reads two DC registers off each slave&apos;s ESC over the wire (FPRD) and returns the fresh
        values — nothing is cached or polled in the background. The browser page re-asks every{' '}
        <strong>2 seconds</strong>. As on the Diagnostics page, this is an EtherCAT-ESC feature — a
        transport without an ESC (e.g. SPoE) reports nothing here.
      </p>

      <p>
        <strong>Roles.</strong> One slave — the first DC-capable one on the bus — becomes the{' '}
        <strong>reference clock</strong>: its local clock <em>is</em> bus time, and it is what every
        other clock is measured against (so its own difference is <code>0 ns</code> by definition).
        Every other DC-capable slave is a <strong>follower</strong> that continuously disciplines its
        local clock toward the reference. A slave with no DC hardware shows <strong>no DC</strong> and
        is not measured.
      </p>

      <p>
        <strong>Propagation</strong> (system-time delay, register <code>0x0928</code>) is how long the
        reference clock&apos;s signal takes to physically reach that slave — the accumulated cable and
        ESC-forwarding latency from the reference to it. The master measures this once during DC
        configuration by timing frame round-trips, and each slave then offsets its local time by its
        own delay so that all of them line up to the <em>same</em> instant despite sitting at different
        points in the chain. It is essentially a measure of topological distance: a slave further down
        the daisy-chain shows a larger propagation delay. It does not normally change unless the
        cabling does.
      </p>

      <p>
        <strong>Difference</strong> (system-time difference, register <code>0x092C</code>) is the live,
        signed deviation of this slave&apos;s local clock from the reference <em>after</em>{' '}
        compensation — the actual &ldquo;am I locked?&rdquo; metric. <strong>Positive</strong> means
        this slave&apos;s clock is running ahead of the reference, <strong>negative</strong> behind.
        Each follower runs a drift-compensation loop that nudges its clock toward the reference, so the
        difference <strong>converges toward zero</strong> over the first moments of exchange and then
        stays within a few tens of nanoseconds. This page treats anything under{' '}
        <strong>1 µs</strong> as locked and flags a slave whose deviation is larger.
      </p>

      <p>
        <strong>Watch the trend, not the instant.</strong> The figure is only meaningful while the bus
        is exchanging in SAFE-OP/OP, because it is the master&apos;s cyclic frame that distributes the
        reference time the followers correct against. So immediately after bring-up — or on an idle bus
        — a follower may legitimately show a large difference that simply hasn&apos;t converged yet.
        The thing that signals a real fault is a deviation that <em>stays</em> large or <em>grows</em>{' '}
        instead of settling: that slave is not locking to the reference.
      </p>

      <p>
        <strong>One important caveat.</strong> This driver brings the bus up{' '}
        <strong>SM-synchronous</strong> (free-run): it measures DC and lets the followers discipline
        their clocks, but it does <em>not</em> activate the <strong>SYNC0</strong> pulse that would
        make each slave act on a hardware tick derived from that synced clock. So the numbers here tell
        you the clocks are aligned, but the drives currently act on frame arrival, not on a SYNC0 edge —
        consistent with the Configuration page showing DC as &ldquo;free-run&rdquo; with cycle/shift at
        0. DC SYNC0 activation is deferred work; until then this page is a health/lock readout rather
        than a sign that hard SYNC0-timed motion is running.
      </p>
    </Explainer>
  )
}
