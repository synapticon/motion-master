import Explainer from './Explainer'

// The teaching panel for the Server → Game Loop page. It explains what the cyclic RT loop is, then
// documents every card on the page — what it means and exactly how it is derived from the two things
// the endpoint returns each poll (cumulative counters + a server timestamp). Kept in sync with the
// tiles rendered by ServerGameLoopPage.
export default function GameLoopExplainer() {
  return (
    <Explainer title="What is the game loop, and what do these numbers mean?">
      <p>
        The <strong>game loop</strong> is the single real-time thread that drives the bus: once per
        cycle it exchanges EtherCAT process data (and, later, steps trajectories) on a fixed{' '}
        <strong>period</strong>. On a real-time host it holds that period almost exactly; on stock
        Windows / macOS the OS timer can be too coarse to sustain a 1&nbsp;ms cycle, and the loop then{' '}
        <strong>skips</strong> grid points to stay phase-locked rather than drift or fire a burst of
        stale frames. This page is a live health readout of that loop — it runs unconditionally, so
        the numbers are meaningful even before a bus is scanned.
      </p>

      <p>
        The endpoint returns <strong>cumulative counters</strong> (totals since the loop started)
        plus the server&apos;s wall-clock <strong>timestamp</strong> at the moment it sampled. The
        page re-polls every <strong>second</strong> and, for the two &ldquo;live&rdquo; figures,{' '}
        <em>diffs</em> this sample against the previous one. Every value is a diagnostic (relaxed,
        unsynchronised) read — treat it as a health signal, not an exact instrument.
      </p>

      <dl className="space-y-3">
        <div>
          <dt className="font-medium text-grey-900">Achieved rate</dt>
          <dd>
            The loop&apos;s <strong>lifetime average</strong> rate:{' '}
            <code>executedCycles ÷ uptime</code>, compared against the target (
            <code>1e6 ÷ periodUs</code>). Because it averages over the whole run, it is smooth and
            slow to move — a brief stall barely dents it. It turns red when it falls below 95% of
            target, which means the host has not been able to keep up on average.
          </dd>
        </div>

        <div>
          <dt className="font-medium text-grey-900">Live rate</dt>
          <dd>
            The <strong>instantaneous</strong> rate over just the last poll:{' '}
            <code>(executedCycles_now − executedCycles_prev) ÷ (timestamp_now − timestamp_prev)</code>
            . This is the canary — a transient stall that the lifetime average would smooth away shows
            up here within one second. It reads <code>—</code> on the first poll (there is no previous
            sample yet to diff against) and resets after a connection error. The small jitter around
            target is just the ~1&nbsp;s poll interval not being exactly one second.
          </dd>
        </div>

        <div>
          <dt className="font-medium text-grey-900">Skipped cycles</dt>
          <dd>
            Total grid points the loop <strong>dropped</strong> to catch up after an overrun or
            stall. Skipping is deliberate: one fresh frame on-grid beats a back-to-back burst of
            stale setpoints the drives can&apos;t use. An occasional skip is harmless; a value that{' '}
            <em>climbs steadily</em> (shown as <code>+N/s</code>) means the configured period is too
            aggressive for this machine — raise it (e.g. to 2&nbsp;ms) so the loop can meet its grid.
          </dd>
        </div>

        <div>
          <dt className="font-medium text-grey-900">Executed cycles</dt>
          <dd>
            Total loop iterations actually <strong>run</strong> since start. Together,{' '}
            <code>executed + skipped</code> is the number of grid points that have elapsed — so
            comparing the two tells you the fraction of grid points the loop is hitting.
          </dd>
        </div>

        <div>
          <dt className="font-medium text-grey-900">Task time</dt>
          <dd>
            How long the <strong>work inside a cycle</strong> takes — the process-data exchange and
            any tasks, excluding the wait for the next deadline. Measured with two steady-clock reads
            around the task loop. It shows the most recent cycle&apos;s time, with the worst-case
            (<strong>max</strong>) and running <strong>average</strong> since start. This is the
            budget being consumed out of each period; if it approaches the period, there is no slack
            left to absorb jitter.
          </dd>
        </div>

        <div>
          <dt className="font-medium text-grey-900">RT scheduling</dt>
          <dd>
            Whether the loop acquired the two real-time privileges at startup.{' '}
            <strong>SCHED_FIFO</strong> is real-time thread priority, so normal work can&apos;t
            preempt the cycle; <strong>mlockall</strong> pins the process&apos;s memory so a page
            fault can&apos;t inject a latency spike (Linux only). Both are best-effort — the loop
            still runs without them, just non-deterministically. They are expected to read{' '}
            <code>no</code> on Windows (never attempted) and on Linux / macOS without{' '}
            <code>CAP_SYS_NICE</code> / <code>CAP_IPC_LOCK</code>.
          </dd>
        </div>
      </dl>
    </Explainer>
  )
}
