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
        <strong>Capture</strong> is about the PC, not about a device. Every request Motion Master
        sends to a device goes out as one EtherCAT frame and comes back as the same frame, in well
        under a millisecond on a good host. The capture program records every frame on the bus
        adapter for the seconds you choose, with the time the kernel gave it, and writes a pcap
        file under <code>captures/</code> in the user cache. The summary matches each frame the
        master sent to the one that came back, by the index and the header of its first datagram,
        and tells a slow PC from a bad cable: a bad cable shows in the port counters above, a slow
        PC shows here and nowhere else.
      </p>
      <ul className="list-disc pl-5 space-y-1">
        <li><strong>Kind</strong> — the cyclic frame carries the process data once per cycle; the control plane is every mailbox and register request. The two are timed apart because a late cyclic frame shows as a short working counter, and a late control-plane frame runs the driver's recovery.</li>
        <li><strong>Sent, Resent</strong> — frames the master sent, and frames sent again with the same index and header while the first copy had no answer. A register request goes out again after 2 ms, so a late one shows as a resend as well as in a band. The return is timed from the first copy.</li>
        <li><strong>Never came back</strong> — sent frames with no return before the capture ended.</li>
        <li><strong>&gt; 2 ms, &gt; 20 ms, &gt; 100 ms</strong> — returns later than each of the three waits the driver uses. The bands are cumulative: a frame later than 100 ms counts in all three. A register request waits 2 ms, then goes out again. A mailbox transfer as a whole waits 20 ms. A mailbox write or read waits 100 ms for its one frame; when the frame is later, the driver counts the write as one with no frame back and recovers it with mailbox counter 0.</li>
        <li><strong>Latest</strong> — the latest return of the capture, after its first send.</li>
      </ul>
      <p>
        The time is the kernel&apos;s, taken at the boundary to the NIC driver on both sides: when
        the frame is handed to the driver, and when the driver hands its return back. So it covers
        the NIC, the wire and the device, and not the PC&apos;s own queues and scheduling between
        that boundary and Motion Master. That is what makes the capture and the server log a pair:
        late frames in the capture put the delay on the wire or in the device, and a clean capture
        beside &ldquo;mailbox write(s) got no frame back&rdquo; warnings puts it inside the PC. A
        pcap from{' '}
        <code>tcpdump</code> on another PC gets the same summary once it is in <code>captures/</code>:
        pick the file with <strong>Summarise a pcap</strong>, or run{' '}
        <code>motion-master-capture --ifname &lt;adapter&gt; --seconds 30 --out frames.pcap</code>{' '}
        on that PC, which ships beside the Motion Master binary and prints the summary itself.
      </p>

      <p>
        Note this whole page is EtherCAT-ESC-specific. A transport with no ESC (e.g. SPoE) has no such
        registers, so it reports no diagnostics here.
      </p>
    </Explainer>
  )
}
