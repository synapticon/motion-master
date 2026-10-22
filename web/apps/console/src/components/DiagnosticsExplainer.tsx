import Explainer from './Explainer'

// The teaching panel for the Fieldbus → Diagnostics page. It explains every counter and flag shown
// per device: where the data comes from (live FPRD reads of the ESC register blocks), the crucial
// "these counters are cumulative and saturate — watch the delta, not the value" caveat, how to read
// the per-port link state and the four error counters to localise a bad cable, the device-level
// processing-unit/PDI errors, and the two watchdogs (including the tick-base math behind the
// process-data watchdog config). Aimed at someone diagnosing a flaky bus.
export default function DiagnosticsExplainer() {
  return (
    <Explainer title="How do I read these diagnostics?">
      <p>
        This page reads the <strong>health registers</strong> built into every EtherCAT Slave
        Controller (ESC) and shows them per device. Unlike the Configuration page (a cached snapshot),
        these are <strong>live</strong> values, read on demand: each time the page asks, Motion Master
        reads them straight off each slave&apos;s ESC over the wire (FPRD register reads) and returns
        the fresh values — nothing is cached or polled in the background. This browser page re-asks
        every <strong>2 seconds</strong>, which is what gives you the live view. They surface a link
        that is starting to degrade — a marginal cable, a loose connector — <em>before</em> it gets bad
        enough to drop a device out of OP.
      </p>

      <p>
        <strong>The single most important thing to understand:</strong> every error counter here is{' '}
        <strong>cumulative since the last power cycle</strong> and 8-bit, so it{' '}
        <strong>saturates at 255</strong> (it stops, it does not wrap) and is only cleared by a power
        cycle or an explicit register write. So an absolute value tells you little — a drive that saw
        12 errors during boot months ago still reads 12. <strong>What matters is whether a counter is
        climbing</strong> as you watch the 2-second refresh. A steady number is history; a rising one
        is a live fault. The per-device header shows <strong>healthy</strong> or an{' '}
        <strong>error count</strong> (the sum of every counter on the card) purely as a quick
        &ldquo;has this device ever logged anything&rdquo; flag — a non-zero count is a prompt to look,
        not proof of a current problem.
      </p>

      <p>
        <strong>Ports.</strong> Each ESC has up to four physical ports (0–3); a slave wired into a
        daisy-chain uses one to face upstream (toward the master) and one to face the next slave
        downstream. The page hides ports that are unused (no link, zero counters) to cut noise. For
        each port in use:
      </p>
      <ul className="list-disc pl-5 space-y-1">
        <li><strong>Link</strong> — is a physical link detected on this port (from DL Status, register <code>0x0110</code>). <em>no</em> on a port you expect to be cabled means a dead/unplugged cable.</li>
        <li><strong>Loop</strong> — <em>closed</em> means nothing is connected downstream (or the port is disabled), so the ESC loops the frame back internally; <em>open</em> means the frame passes through to a downstream slave. This is how the ring is formed and where it turns around.</li>
        <li><strong>Comm</strong> — stable communication is established on this port (DL Status). A port with a link but no comm is a port struggling to synchronise.</li>
        <li><strong>Invalid</strong> (<code>0x0300+</code>) — frames arriving with a bad checksum (FCS) or malformed structure. The headline &ldquo;this port received corruption&rdquo; counter.</li>
        <li><strong>RX err</strong> (<code>0x0301+</code>) — physical-layer receive errors: the PHY asserted RX_ER (bad symbols on the wire). Points at the cable/connector feeding <em>this</em> port.</li>
        <li><strong>Fwd err</strong> (<code>0x0308+</code>) — forwarded errors: frames that were <em>already flagged as bad by an upstream ESC</em> before reaching here. This device merely counted an error someone else first detected.</li>
        <li><strong>Lost link</strong> (<code>0x0310+</code>) — how many times this port&apos;s link dropped and came back. A counter that ticks up points squarely at intermittent cabling.</li>
      </ul>

      <p>
        <strong>Localising a bad segment</strong> is the whole point of the per-port split, and the
        key is the difference between <strong>RX err / Invalid</strong> and <strong>Fwd err</strong>.
        Errors first appear as RX/Invalid on the port that physically received the corruption; that
        same frame, now marked bad, is forwarded down the chain and shows up as <strong>Fwd err</strong>{' '}
        on every device after it. So the segment to suspect is the one where you see RX/Invalid climbing{' '}
        <em>without</em> a corresponding upstream Fwd err — the device <em>downstream</em> of the bad
        cable, on the port facing it. Forwarded errors are echoes; rising RX/Invalid is the source.
      </p>

      <p>
        <strong>Device-level counters</strong> (above the ports) cover faults inside the slave rather
        than on a specific cable:
      </p>
      <ul className="list-disc pl-5 space-y-1">
        <li><strong>Processing-unit err</strong> (<code>0x030C</code>) — datagrams that reached the ESC&apos;s processing unit malformed (got past the ports but were unusable).</li>
        <li><strong>PDI err</strong> (<code>0x030D</code>) — errors on the Process Data Interface, the internal bus between the ESC and the slave&apos;s own microcontroller. Points inward, at the slave&apos;s local hardware/firmware, not at the EtherCAT wire.</li>
      </ul>

      <p>
        <strong>The watchdogs</strong> are dead-man&apos;s switches, and there are two. The{' '}
        <strong>process-data (SM) watchdog</strong> is the important one: a device in OP that stops
        seeing fresh process data for longer than its timeout faults <em>itself</em> from OP down to
        SAFE-OP with an error — a safety reflex so a drive whose master went silent doesn&apos;t keep
        acting on stale targets. The <strong>PDI watchdog</strong> guards the internal ESC↔microcontroller
        interface instead. Each has an <strong>expirations</strong> counter (process-data{' '}
        <code>0x0442</code>, PDI <code>0x0443</code>) — how many times it has fired since power-up.
      </p>

      <p>
        The <strong>Process-data watchdog</strong> panel shows that timeout&apos;s live state and lets
        you change it:
      </p>
      <ul className="list-disc pl-5 space-y-1">
        <li><strong>State</strong> (<code>0x0440</code> bit 0) — <em>Running</em> (counting normally, being fed), <em>Expired</em> (timed out, or not yet fed since enable), or <em>Disabled</em> (timeout 0).</li>
        <li><strong>Timeout</strong> — the configured time (<code>0x0420</code>) before expiry; on expiry an OP device drops to SAFE-OP+error.</li>
        <li>
          <strong>Derivation</strong> — the timeout is not stored in milliseconds; it is{' '}
          <em>ticks × a tick base</em>. The tick base is <code>(divider + 2) × 40 ns</code>, where{' '}
          <code>40 ns</code> is one period of the ESC&apos;s 25 MHz reference clock and the divider
          (<code>0x0400</code>) sets how many such periods make one watchdog tick (the <code>+2</code>{' '}
          is a fixed hardware offset). This row makes the arithmetic transparent, since a timeout you
          set is rounded to a whole number of ticks.
        </li>
        <li><strong>Expirations</strong> — the same <code>0x0442</code> counter described above, shown here next to the config that governs it.</li>
      </ul>
      <p>
        Setting <strong>0 ms disables</strong> the watchdog. The main reason to <em>raise</em> it: a
        whole-bus process-image re-map briefly pauses PDO for every device (see the Process Image
        page), and a too-tight watchdog can trip a staying-in-OP device during that pause — a larger
        timeout lets it ride through. Configuration is per-device (it lives in the slave&apos;s ESC), so
        this panel has its own read/write to that slave rather than going through the page&apos;s bulk
        fetch.
      </p>

      <p>
        <strong>Frames</strong> is about the PC, not about a device. Every request Motion Master
        sends to a device goes out as one EtherCAT frame and comes back as the same frame, in well
        under a millisecond on a good host. The driver stamps every mailbox and register datagram
        when it goes out and times the frame when it comes back, also a frame that comes back after
        the driver stopped waiting for it. The counts are since the bus came up or the last Reset.
        This is the number that tells a slow PC from a bad cable: a bad cable shows in the port
        counters above, a slow PC shows here and nowhere else.
      </p>
      <ul className="list-disc pl-5 space-y-1">
        <li><strong>Measured</strong> — frames that came back and were timed.</li>
        <li><strong>Later than 2 ms, 20 ms, 100 ms</strong> — how many frames came back later than each of the three waits the driver uses. The bands are cumulative: a frame later than 100 ms counts in all three. A register read or write waits 2 ms, then goes out again. A mailbox transfer as a whole waits 20 ms. A mailbox write or read waits 100 ms for its one frame; when the frame is later than that, the driver counts the write as one with no frame back and recovers it with mailbox counter 0, which is the <em>Writes without a frame back</em> and <em>Requests sent with counter 0</em> table below the tiles.</li>
        <li><strong>Never came back</strong> — no frame within 500 ms. The driver has 32 frame indexes, one per frame on the wire at a time. An index whose frame has not come back is kept out of use for 500 ms so that a late frame is still timed; a frame later than that counts here, and so does one whose index had to be used again because all 32 were in use.</li>
        <li><strong>Latest</strong> — the latest return since the counts were zero. It is exact while a thread was waiting for the frame. A frame nobody was waiting for sits in the socket until the next receive by any thread, so for such a frame the time is an upper bound.</li>
      </ul>
      <p>
        The time is the round trip as Motion Master sees it, from just before the send call to just
        after the receive call returned the frame. It says how late a frame was, not where the delay
        was: the NIC, the kernel, the wire and the device are all in it. The cyclic process-data
        frame is not in these counts; a late one shows on the Process Image page as a short working
        counter. The server also writes one warning to its log, and publishes a{' '}
        <code>late-frame</code> notification, the first time the latest return passes 20 ms and
        the first time it passes 100 ms. <strong>Reset</strong> sets every count to zero so that one
        test run can be measured on its own.
      </p>

      <p>
        Note this whole page is EtherCAT-ESC-specific. A transport with no ESC (e.g. SPoE) has no such
        registers, so it reports no diagnostics here.
      </p>
    </Explainer>
  )
}
