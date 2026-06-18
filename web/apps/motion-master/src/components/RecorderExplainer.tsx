import Explainer from './Explainer'

// The teaching panel for the Data → Recorder page. It explains what the lossless process-data
// recorder is (a circular ring the real-time loop appends one record to every cycle), that it is
// the single source feeding both the live Monitoring stream and the .mmpd dump, what a dump
// contains and why it is decodable fully offline, the three actions on this page, and how to read
// the chart and cycle-time stats. It ties back to the Process Image (the layout embedded as the
// dump header) and Monitoring (the live view of the same ring) pages.
export default function RecorderExplainer() {
  return (
    <Explainer title="What is the recorder?">
      <p>
        The <strong>process-data recorder</strong> is a circular ring in memory that the real-time
        loop appends <em>one record to every cycle</em> — the full raw input and output images, an
        epoch-nanosecond timestamp, and the working counter. Nothing is sampled or down-rated: every
        cyclic exchange the master performs is captured, which is why it is <strong>lossless</strong>.
        The ring is sized by duration (300&nbsp;s by default), so it always holds the most recent
        window of bus traffic and overwrites the oldest cycles as it fills.
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

      <p>This page gives you three ways to work with it:</p>
      <ul className="list-disc pl-5 space-y-1">
        <li>
          <strong>Record &amp; view</strong> — streams the current ring contents into the browser and
          plots them immediately. The fastest way to look at what just happened on the bus.
        </li>
        <li>
          <strong>Open .mmpd file</strong> — loads and decodes a dump you saved earlier (or someone
          sent you), entirely client-side. Useful for sharing a capture or revisiting one later.
        </li>
        <li>
          <strong>Dump to disk</strong> — writes a <code>.mmpd</code> file on the machine running
          Motion Master and reports its path. This is the path for terminal/headless users who have no
          browser to stream into.
        </li>
      </ul>

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
