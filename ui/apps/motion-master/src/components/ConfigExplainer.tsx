import Explainer from './Explainer'

// The teaching panel for the Fieldbus → Configuration page. It explains what each value on a slave
// card means — the process-data sizes, the mailbox (and why BOOT's is larger), the supported
// protocols, the distributed clock, and the Sync Managers and FMMUs — and, crucially, how those
// last two relate to turn an object-dictionary mapping into bytes on the wire. Everything here is
// the *static* ESC configuration the master programmed during the last scan; it is read from cached
// state with no bus I/O, so it reflects the slave as last configured, not a live readback.
export default function ConfigExplainer() {
  return (
    <Explainer title="What am I looking at?">
      <p>
        Every EtherCAT slave contains an <strong>EtherCAT Slave Controller (ESC)</strong> — the chip
        that sits on the wire, processes each passing frame, and hands data to and from the slave&apos;s
        application. This page is a snapshot of how the master <em>configured</em> that ESC during the
        last scan: the addresses, the process-data sizes, the mailbox windows, the distributed clock,
        and the two pieces of ESC hardware — <strong>Sync Managers</strong> and <strong>FMMUs</strong>{' '}
        — that actually move data between the bus and the slave&apos;s memory. It is read from cached
        state with no bus traffic, so it describes the slave as last configured, not a live readback.
      </p>

      <p>
        Almost all of it originates in the slave&apos;s own <strong>SII (EEPROM)</strong>: the slave
        tells the master, in a fixed set of EEPROM words read during the scan, where its mailbox lives,
        which protocols it speaks, and how its memory is laid out. The master programs the ESC
        accordingly. So this page is really &ldquo;what the slave asked for, and what the master then
        set up.&rdquo;
      </p>

      <p>
        <strong>Output / Input</strong> are the sizes of the cyclic <strong>process data</strong> in
        each direction, counted in bits. <strong>Output</strong> is master→slave (the{' '}
        <strong>RxPDO</strong>: targets the master sends every cycle — controlword, target position,
        modes of operation, …). <strong>Input</strong> is slave→master (the <strong>TxPDO</strong>:
        feedback the slave returns — statusword, actual position, actual torque, …). These sizes are
        the sum of the objects selected in the device&apos;s <strong>PDO mapping</strong>; change the
        mapping and these numbers change. Process data is the high-rate channel exchanged in lockstep
        with the real-time loop, in contrast to the mailbox below.
      </p>

      <p>
        The <strong>Mailbox</strong> is the acyclic, request/response channel — used for{' '}
        <strong>CoE/SDO</strong> parameter access, <strong>FoE</strong> file transfer, and so on. It
        has two windows, each shown here as <em>length</em> @ <em>physical ESC offset</em>: a{' '}
        <strong>write</strong> window (master→slave) and a <strong>read</strong> window (slave→master).
        Both the sizes and offsets come from the SII EEPROM, and the slave is reached through this
        channel from PRE-OP upward — not in INIT (no mailbox) and not as process data.
      </p>

      <p>
        <strong>Why is the mailbox bigger in BOOT than in PRE-OP?</strong> Because a slave defines{' '}
        <em>two</em> mailbox configurations in its EEPROM: a <strong>standard mailbox</strong> (used
        from PRE-OP up, sized for small CoE/SDO messages) and a separate{' '}
        <strong>bootstrap mailbox</strong> (used only in BOOT). BOOT exists for firmware download over
        FoE, and a bigger mailbox lets each firmware chunk be larger, so the transfer finishes faster —
        e.g. <code>1024</code> bytes in BOOT versus <code>128</code> in PRE-OP on some SOMANET devices.
        When the master takes a slave to BOOT it reprograms the mailbox Sync Managers from the
        bootstrap EEPROM words; on the way back to PRE-OP it restores them from the standard words.
        The size shown on this card is whichever the master last programmed — after a fresh scan, the
        standard (PRE-OP) mailbox.
      </p>

      <p>
        <strong>Protocols</strong> lists the mailbox protocols the slave advertises — a bitfield read
        straight from the SII EEPROM&apos;s mailbox-protocol word:
      </p>
      <ul className="list-disc pl-5 space-y-1">
        <li><strong>CoE</strong> — CANopen over EtherCAT: the object dictionary, SDO parameter access, PDO mapping. The one SOMANET drives rely on.</li>
        <li><strong>FoE</strong> — File over EtherCAT: firmware and file transfer (used in BOOT).</li>
        <li><strong>EoE</strong> — Ethernet over EtherCAT: tunnels standard Ethernet/IP frames.</li>
        <li><strong>SoE</strong> — Servo-drive (SERCOS) profile over EtherCAT.</li>
        <li><strong>AoE</strong> — ADS over EtherCAT (Beckhoff ADS routing).</li>
        <li><strong>VoE</strong> — Vendor-specific over EtherCAT.</li>
      </ul>
      <p>
        A protocol appearing here only means the slave <em>supports</em> it; Motion Master drives
        SOMANET hardware over CoE and FoE.
      </p>

      <p>
        <strong>Distributed clock (DC)</strong> is EtherCAT&apos;s bus-wide time synchronisation. One
        slave — the first DC-capable one — becomes the <strong>reference clock</strong> that defines
        bus time; every other DC slave continuously disciplines its local clock toward it. The{' '}
        <strong>propagation delay</strong> shown here is how long a frame takes to reach that slave,
        measured by the master during DC setup, so each slave can compensate for its position in the
        chain. <strong>SYNC0</strong> is a hardware pulse a slave can generate from its synced clock to
        trigger its application at an exact instant (with the <strong>cycle</strong> and{' '}
        <strong>shift</strong> defining that pulse&apos;s timing). This driver uses{' '}
        <strong>SM-synchronous</strong> bring-up — the slave acts on each arriving process-data frame
        rather than on a SYNC0 pulse — so DC is measured but SYNC0 is left off, and cycle/shift read{' '}
        <code>0</code>.
      </p>

      <p>
        <strong>Sync Managers (SM)</strong> are ESC hardware units that each guard a window of the
        slave&apos;s physical memory and arbitrate access to it between the master (over the wire) and
        the slave&apos;s application, so the two never read a half-written buffer. Each runs in one of
        two modes:
      </p>
      <ul className="list-disc pl-5 space-y-1">
        <li>
          <strong>Mailbox mode (1 buffer, handshake)</strong> — for the acyclic mailbox, where every
          message must arrive intact and none may be lost. The SM holds a single buffer with a
          full/empty flag and enforces strict flow control: the writer may only write when the buffer
          is <em>empty</em>, and writing completes the moment the <em>last</em> byte of the window is
          touched — at which point the SM flips the buffer to <em>full</em> and locks out further
          writes. The reader empties it by reading to that same last byte, which flips it back to
          empty and re-enables the writer. Because the flag turns only on access to the end address, a
          message is always transferred whole — never half-read or overwritten mid-message. If a
          writer tries while the buffer is still full, the access is refused (the working counter stays
          0) and it retries; a toggle/repeat bit lets the two sides recover a lost or duplicated
          message. Reliable, but only one message is in flight at a time. Typically{' '}
          <strong>SM0</strong> = Mailbox Out (master→slave) and <strong>SM1</strong> = Mailbox In
          (slave→master).
        </li>
        <li>
          <strong>Buffered / 3-buffer mode (&ldquo;free-run&rdquo;)</strong> — for cyclic process data,
          where only the <em>freshest</em> value matters and <em>neither side may ever block</em>.
          Behind one logical address the SM keeps three physical buffers and rotates them: at any
          instant one is being written, one holds the last completed copy ready to read, and one is
          spare. When the writer finishes a buffer (again, on touching the last byte) the SM atomically
          hands it over as the new &ldquo;latest complete&rdquo; copy and gives the writer a fresh free
          buffer for the next cycle — so the writer never waits for the reader, and the reader always
          gets a whole, most-recent frame, never a torn one. The trade-off is the opposite of the
          mailbox&apos;s: nothing blocks and nothing tears, but if the writer produces two frames
          before the reader takes one, the intermediate frame is simply skipped — exactly what you want
          for a setpoint or a feedback sample, where last-value-wins is correct. This is what lets the
          master read/write the whole bus every cycle without ever stalling on a slave. Typically{' '}
          <strong>SM2</strong> = Outputs (RxPDO) and <strong>SM3</strong> = Inputs (TxPDO).
        </li>
      </ul>
      <p>
        The two modes are the same hardware tuned for opposite goals:{' '}
        <strong>mailbox = lossless and ordered but one-at-a-time</strong> (reliability), whereas{' '}
        <strong>3-buffer = always-fresh and never-blocking but lossy of intermediate values</strong>{' '}
        (timeliness). Each SM row on the card shows the SM&apos;s role, the physical address and length
        of the window it guards, and its raw control/flags register (buffer mode, direction, watchdog,
        enable).
      </p>

      <p>
        <strong>FMMUs (Fieldbus Memory Management Units)</strong> are the other half of the picture.
        The master addresses the <em>entire bus</em> as one big <strong>logical</strong> address space
        and reads/writes all slaves&apos; process data in a single cyclic datagram. An FMMU on each
        slave maps a slice of that bus-wide logical space onto the slave&apos;s local{' '}
        <strong>physical</strong> memory — bit-for-bit, which is why the rows carry both a logical and
        a physical bit range. In short: the <strong>Sync Manager</strong> guards the physical buffer;
        the <strong>FMMU</strong> wires that physical buffer into the shared logical address the master
        actually talks to. A slave usually has one FMMU for outputs, one for inputs, and sometimes one
        that maps a mailbox SM&apos;s status byte into logical space so the master can poll
        &ldquo;mailbox full?&rdquo; in the same cyclic frame.
      </p>

      <p>
        <strong>How it all fits together.</strong> The device&apos;s object dictionary defines a{' '}
        <strong>PDO mapping</strong> (which objects are exchanged cyclically) → that sets the{' '}
        <strong>Output/Input</strong> bit sizes → a process-data <strong>Sync Manager</strong> guards
        the physical buffer holding those bytes → an <strong>FMMU</strong> maps that buffer into the
        master&apos;s logical address space → the real-time loop exchanges the whole bus&apos;s logical
        image every cycle. The Process Image page shows the result of that chain — the exact byte
        layout the loop exchanges — while this page shows the ESC plumbing underneath it.
      </p>
    </Explainer>
  )
}
