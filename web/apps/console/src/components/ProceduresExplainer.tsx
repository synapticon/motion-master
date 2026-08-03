import Explainer from './Explainer'

// The "What is a procedure?" teaching panel for the per-device Procedures page. It explains the
// lifecycle a client has to drive — start, poll, cancel — and the guarantees that make polling
// sufficient, because this page is also the reference for anyone building their own UI against the
// same endpoints.
export default function ProceduresExplainer() {
  return (
    <Explainer title="What is a procedure?">
      <p>
        A <strong>procedure</strong> is an operation you ask this device to carry out, which Motion
        Master runs as a <strong>background job</strong> rather than as one request that blocks until
        it is finished. Starting one returns immediately with the run already under way, and you read
        its state by asking again.
      </p>

      <p>
        <strong>Every procedure has the same shape, however long it takes.</strong> Some are done
        before your first read and simply report as succeeded; others work through several steps and
        fill them in as they go. That uniformity is the point — one way to start, one way to follow,
        one way to cancel — so a client drives all of them with the same loop instead of needing to
        know which kind it is looking at.
      </p>

      <p>
        While a procedure runs, the rest of Motion Master keeps working: parameter reads still answer,
        monitoring keeps streaming, and the real-time loop keeps cycling. Only a bus <em>scan</em>, a{' '}
        <em>reset</em>, and an <strong>AL state change</strong> have to wait — those rebuild or re-map
        what the running procedure is holding.
      </p>

      <p>
        <strong>One procedure at a time per device.</strong> A second start on a device that is
        already running one is refused (HTTP 409) rather than queued. The claim is held for the whole
        span and released when the run ends — success, failure or cancellation alike. Different
        devices are independent.
      </p>

      <p>
        <strong>Progress is polled, and polling cannot miss a result.</strong> There is no WebSocket
        for this. Each read returns the <em>whole</em> run — every step with its status and whatever
        it measured — rather than a stream of events, so a step that both starts and finishes between
        two of your polls is still reported as succeeded, with its value. That is what makes a plain{' '}
        <code>while (status === &apos;running&apos;)</code> loop lossless. This page polls a few
        times a second while something is running, and not at all when nothing is.
      </p>

      <p>
        <strong>The result outlives the run.</strong> Leave this page and come back and you still see
        how the last run went, with the times it started and finished — a stale measurement presented
        as current would be worse than none. <code>runCount</code> is the generation counter: it
        increments on every accepted start, so if it changed since your last poll you are looking at
        a different run (there is no run id, and <code>(device, procedure, runCount)</code> is a
        stable identifier if you need one). Results are dropped on a scan or reset, because bus
        positions may then name different hardware.
      </p>

      <p>
        <strong>Cancelling stops the run, not the record.</strong> The snapshot stays behind and
        reports how far it got, with status <code>cancelled</code> — deliberately distinct from{' '}
        <code>failed</code>, because &ldquo;I stopped it&rdquo; and &ldquo;the drive could not do
        it&rdquo; are different outcomes. Where the procedure is waiting on the drive it also tells
        the drive to abort, so a run stops when you ask rather than carrying on to completion.
      </p>

      <p>
        <strong>What a device offers depends on the device.</strong> The list on the left is served by
        Motion Master, not built into this page, so it stays in step with the server as procedures
        are added — and a procedure appears only where it applies. A third-party slave will show none.
      </p>
    </Explainer>
  )
}
