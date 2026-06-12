import Explainer from './Explainer'

// The "What happens on the bus?" teaching panel for the Fieldbus → Control page. It explains, at
// the EtherCAT level, what each action on this page actually does — binding the master to the NIC
// (Init), enumerating slaves (Scan), and driving the per-slave Application-Layer state machine
// (Transition) — so the buttons aren't black boxes.
export default function ControlExplainer() {
  return (
    <Explainer title="What happens on the bus?">
      <p>
        This page drives the EtherCAT master itself: it brings the master onto the wire, discovers
        the slaves attached to it, and commands the <strong>Application Layer (AL) state machine</strong>{' '}
        that every slave runs. EtherCAT is raw layer-2 Ethernet — no IP, no switching — so the
        master talks to the slaves directly over the network adapter you select.
      </p>

      <p>
        <strong>Init</strong> binds the fieldbus driver to a network adapter. Motion Master opens a
        raw Ethernet socket on that NIC (this needs the <code>CAP_NET_RAW</code> capability), puts it
        in promiscuous mode, and prepares the master to send and receive EtherCAT frames on it. The{' '}
        <strong>driver</strong> chooses how those frames reach the slaves — <strong>SOEM</strong> for
        a standard NIC, <strong>SPoE</strong> for SOMANET-Protocol-over-Ethernet, <strong>IgH</strong>{' '}
        for the IgH master. Init touches no slaves and sends no bus traffic; it only establishes the
        master. <strong>Reset</strong> is the inverse: it tears the driver down, releases the socket,
        and clears the device list, so Init must be run again afterwards.
      </p>

      <p>
        <strong>Scan</strong> enumerates the bus. The master sends a broadcast datagram down the
        line; because EtherCAT slaves are wired in a daisy-chain and each one processes the frame
        on the fly and increments a working counter as it passes, the master learns how many slaves
        are present and in what physical order. Each slave is then assigned a fixed{' '}
        <strong>station address</strong> — a 16-bit number the master writes into the slave&apos;s ESC
        so it can address that slave directly on the wire, rather than by counting hops during the
        initial scan (the Configuration page shows it as the <code>@ 0x…</code> value next to each
        slave).
        Its <strong>SII (EEPROM) identity</strong> is read — vendor ID, product code, revision,
        serial number, name — and its mailbox and default Sync-Manager / FMMU configuration are set
        up from that SII. Scanning is destructive on both sides: it resets every slave to{' '}
        <strong>INIT</strong>, and it rebuilds Motion Master&apos;s device list from scratch — it is
        the only action that <em>repopulates</em> the list (Reset also clears it, but leaves it empty).
      </p>

      <p>
        <strong>Refresh</strong> (next to the device list, and in the sidebar) is the non-destructive
        counterpart: it re-reads the existing device list and each slave's current AL state without
        re-scanning, so slaves keep whatever state they are in (e.g. PRE-OP or OP). Use Scan when the
        cabling changed; use Refresh just to update what is shown.
      </p>

      <p>
        <strong>Transition to State</strong> commands the AL state machine. The master writes the
        requested state into each slave's <strong>AL Control</strong> register (<code>0x0120</code>);
        the slave performs the transition and reports the outcome in its <strong>AL Status</strong>{' '}
        register (<code>0x0130</code>), and — if it refuses — an <strong>AL Status Code</strong>{' '}
        (<code>0x0134</code>) saying why. The states differ by what communication they enable:
      </p>
      <ul className="list-disc pl-5 space-y-1">
        <li>
          <strong>INIT</strong> — no mailbox, no process data. The starting point; reachable directly
          from any state. ESC <strong>register</strong> read/write and <strong>SII (EEPROM)</strong>{' '}
          read/write still work here — they ride the data-link layer, not the mailbox — and EEPROM
          access is in fact most reliable in INIT.
        </li>
        <li>
          <strong>PRE-OP</strong> — mailbox communication (CoE / SDO) is live; no process data yet.
          This is where parameters are read and written.
        </li>
        <li>
          <strong>SAFE-OP</strong> — inputs (TxPDO) are exchanged cyclically and the mailbox stays
          active, but outputs are held in their safe state, not applied.
        </li>
        <li>
          <strong>OP</strong> — everything active: inputs, outputs, and mailbox. The drive is fully
          live and following commanded targets.
        </li>
        <li>
          <strong>BOOT</strong> — a special firmware-download state with only the FoE mailbox
          available (no CoE, no process data). Reachable only from INIT, and it only returns to INIT.
        </li>
      </ul>

      <p>
        <strong>The transitions are not free-form.</strong> Climbing must be step by step
        (INIT → PRE-OP → SAFE-OP → OP), but you can drop several states at once (e.g. OP → INIT), and
        BOOT pairs only with INIT. Illegal jumps are rejected by the slave with AL status code{' '}
        <code>0x0011</code>, “Invalid requested state change”. Motion Master also rejects them up
        front, before commanding the bus.
      </p>

      <p>
        <strong>Motion Master reacts to the state you ask for.</strong> Changing AL state is your
        decision; the master responds to it. Entering SAFE-OP or OP, it reads each targeted slave's
        PDO mapping over the mailbox and (re)builds the whole-bus <strong>process image</strong> — the
        memory layout the real-time loop exchanges every cycle — then starts exchanging. Leaving those
        states, it tears the image down only once no slave will remain exchanging, so a subset of
        devices can be taken to BOOT (for firmware) or PRE-OP (to re-map) while the rest keep running.
        Bringing them back re-maps the whole bus, which briefly pauses exchange for everyone — the
        accepted cost of bringing a device online.
      </p>
    </Explainer>
  )
}
