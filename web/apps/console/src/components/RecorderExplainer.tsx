import Explainer from './Explainer'

// The teaching panel for the Data → Recorder page. It explains what the lossless process-data
// recorder is (a circular ring the real-time loop appends one record to every cycle), that its
// depth is a cycle count rather than a duration (so the window it covers moves with the loop
// period), that it is the single source feeding both the live Monitoring stream and the .mmpd
// dump, what a dump contains and why it is decodable fully offline, the four actions on this page
// — organised by *where the dump ends up*, which is what actually distinguishes them — and how to
// read the chart and cycle-time stats. It ties back to Process Image (the layout embedded as the
// dump header), Monitoring (the live view of the same ring), and Storage → User Cache (where saved
// dumps are stored alongside everything else Motion Master keeps on disk).
export default function RecorderExplainer() {
  return (
    <Explainer title="What is the recorder?">
      <p>
        The <strong>process-data recorder</strong> is a circular ring in memory that the real-time
        loop appends <em>one record to every cycle</em> — the full raw input and output images, an
        epoch-nanosecond timestamp, and the working counter. Nothing is sampled or down-rated: every
        cyclic exchange the master performs is captured, which is why it is <strong>lossless</strong>.
        It always holds the most recent window of bus traffic, overwriting the oldest cycles as it
        fills.
      </p>

      <p>
        The ring is sized in <strong>cycles</strong>, not seconds — 300,000 of them by default
        (<code>recorder.capacity</code>). How much wall-clock time that covers therefore depends on
        the loop period: about <strong>5 minutes at a 1&nbsp;ms cycle</strong>, but 20 minutes at
        4&nbsp;ms (the default on Windows). Memory follows the same arithmetic — roughly 38&nbsp;MB
        per drive at the default capacity — so raising it on a multi-drive bus costs real RAM, which
        is pinned for the real-time loop.
      </p>

      <p>
        It is the <strong>single source</strong> for two things: the live{' '}
        <strong>Monitoring</strong> stream (which ships every recorded cycle since each monitoring&apos;s
        last flush — also lossless) and the <code>.mmpd</code> dumps on this page. Both read the same
        ring; neither re-reads the bus.
      </p>

      <p>
        <strong>What is a <code>.mmpd</code> dump?</strong> A snapshot of every cycle currently in the
        ring, written oldest→newest at the moment you act, with the current{' '}
        <strong>process-image layout embedded as a header</strong>. Because the layout travels with the
        data, a dump is decodable <em>fully offline</em> — the viewer here knows which bytes are which
        object without touching a live bus. A dump can be taken in any state, including while the bus
        is actively exchanging in OP.
      </p>

      <p>
        The actions on this page differ mainly in <em>where the dump ends up</em>, which is the thing
        worth getting right before you take one:
      </p>
      <ul className="list-disc pl-5 space-y-1">
        <li>
          <strong>Record &amp; view</strong> — streams the current ring straight into this browser and
          plots it. Nothing is stored anywhere; close the tab and it is gone. The fastest way to look
          at what just happened on the bus.
        </li>
        <li>
          <strong>Dump to disk</strong> — writes a <code>.mmpd</code> file on the machine running
          Motion Master and <em>keeps</em> it, until you delete it. Use this for a capture you want to
          come back to, compare against, or hand to someone else.
        </li>
        <li>
          <strong>Saved dumps → Open</strong> — decodes one of those stored dumps into the chart. The
          file is read on the server, so this works even when the browser has no access at all to that
          machine&apos;s filesystem — the usual situation when Motion Master runs on an appliance
          across the network.
        </li>
        <li>
          <strong>Open .mmpd file</strong> — loads a dump from <em>this</em> computer instead, entirely
          client-side. For a capture someone sent you, or one you downloaded earlier.
        </li>
      </ul>

      <p>
        Saved dumps live in the <code>dumps/</code> folder of Motion Master&apos;s user cache, and are
        listed alongside everything else it stores under <strong>Storage → User Cache</strong>. They
        are the same size as the ring they came from — tens of megabytes per drive — and{' '}
        <strong>nothing deletes them for you</strong>, so a machine that dumps regularly needs clearing
        out occasionally. Deliberately not the system temporary directory, which the OS reaps on a
        timer: a capture you meant to keep would quietly disappear.
      </p>

      <p>
        Once a recording is loaded, tick objects in the per-device picker to plot them. Only{' '}
        <strong>numeric</strong> objects are plottable — strings and raw byte blobs are shown but
        disabled. The chart&apos;s x-axis is microseconds elapsed since the first recorded cycle, so it
        lines up with the live monitoring chart.
      </p>

      <p>
        The <strong>cycle-time stats</strong> above the chart are computed from the recording&apos;s own
        timestamps — the same jitter readout as live monitoring — so they reflect how steadily the loop
        actually ran while these cycles were captured, independent of which series you picked.
      </p>

      <p>
        For the byte-level <em>layout</em> behind a dump see the <strong>Process Image</strong> page;
        for a continuously updating view of the same ring see <strong>Monitoring</strong>. Here you are
        looking at a captured span of traffic, frozen for inspection.
      </p>
    </Explainer>
  )
}
