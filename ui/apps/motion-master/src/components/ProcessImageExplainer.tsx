import Explainer from './Explainer'

// The teaching panel for the Fieldbus → Process Image page. It explains what the process image is
// (the flat, whole-bus memory the real-time loop exchanges every cycle), how it is built from each
// device's PDO mapping, how to read the Outputs/Inputs tables and the byte/bit offsets, what a
// generation is, and how the working counter signals per-cycle bus health. It deliberately ties
// back to the Configuration page (the SM/FMMU plumbing that produces this layout).
export default function ProcessImageExplainer() {
  return (
    <Explainer title="What is the process image?">
      <p>
        The <strong>process image</strong> is the flat block of memory the EtherCAT master exchanges
        with the <em>entire bus</em> on every real-time cycle. The master doesn&apos;t talk to slaves
        one at a time during cyclic operation — it lays all of their process data out in one shared{' '}
        <strong>logical address space</strong> and reads/writes the whole thing with a single datagram
        per cycle. This page shows that layout: which object from which device lands at which byte, how
        big each section is, and whether the last exchange was healthy.
      </p>

      <p>
        It comes in two independent halves. <strong>Outputs · RxPDO</strong> is master→slave — the
        targets the master sends every cycle (controlword, target position/velocity/torque, modes of
        operation, …). <strong>Inputs · TxPDO</strong> is slave→master — the feedback the slaves
        return (statusword, actual position, actual torque, error codes, …). They are exchanged as two
        separate images, so each is offset from <code>0</code> independently: an input object at byte 0
        sits at the start of the <em>input</em> image, not after the outputs.
      </p>

      <p>
        Each row places one mapped object dictionary entry in its direction&apos;s image:
      </p>
      <ul className="list-disc pl-5 space-y-1">
        <li><strong>Object</strong> — the CoE entry being mapped, as <code>index:subindex</code> (e.g. <code>0x6064:00</code>, actual position).</li>
        <li><strong>Byte</strong> — its byte offset within that direction&apos;s image.</li>
        <li><strong>Bit</strong> — its bit offset within that byte, for objects packed below byte granularity (a 1-bit flag sharing a byte with its neighbours).</li>
        <li><strong>Bits</strong> — the object&apos;s width. Sum the widths in a direction and you get that section&apos;s size.</li>
      </ul>
      <p>
        The set of objects, and therefore the whole layout, is the device&apos;s{' '}
        <strong>PDO mapping</strong> — the entries it lists in its object dictionary as exchangeable
        cyclically. Re-map the PDOs (or update firmware that changes them) and these rows change.
      </p>

      <p>
        <strong>Where does this layout come from?</strong> When you command a device to SAFE-OP or OP,
        the master builds this image <em>before</em> actually moving it there — while it is still in
        PRE-OP. It reads each device&apos;s PDO mapping over the mailbox (only possible from PRE-OP up),
        concatenates every device&apos;s outputs into one output image and every device&apos;s inputs
        into one input image, and programs the per-slave <strong>Sync Managers and FMMUs</strong> (see
        the Configuration page) so each slave&apos;s physical buffer is wired into its slice of this
        shared logical image — and only then writes the AL state to complete the transition. The order
        is required: the slave rejects SAFE-OP unless its process-data mapping is already in place. The
        result is what you see here. So the <strong>Configuration</strong> page shows the ESC plumbing; this page
        shows the byte layout that plumbing produces.
      </p>

      <p>
        <strong>Generations.</strong> The image is rebuilt from scratch each time a device enters or
        leaves SAFE-OP/OP — every rebuild is a new <strong>generation</strong>. The whole-bus image is
        mapped in one shot, so a rebuild briefly pauses exchange for everyone; that is the accepted cost
        of bringing a device online. When the last exchanging device leaves SAFE-OP/OP the live image is
        torn down, but the most recent generation is <em>retained</em> so the layout stays inspectable
        here even while the bus is idle (working-counter health then no longer applies).
      </p>

      <p>
        <strong>Working counter (WKC).</strong> This is EtherCAT&apos;s built-in per-cycle health
        signal. Every datagram carries a counter that each slave increments as the frame passes{' '}
        <em>if</em> it successfully processed the part addressed to it: <strong>+1 for a successful
        read</strong> (it returned its inputs) and <strong>+2 for a successful write</strong> (it
        accepted its outputs) — so a drive doing both adds <strong>3</strong>. The master sums what it
        expects from every exchanging device (<code>expected</code>) and compares it to what actually
        came back (<code>last</code>). A match means every device exchanged this cycle; a shortfall
        means one dropped out or stopped processing — the WKC pinpoints that something is wrong even
        before a device reports an AL fault.
      </p>

      <p>
        Note this page shows the <em>layout and health</em>, not the live values flowing through it.
        The actual cyclic values are recorded every cycle and streamed on the Monitoring page (and
        captured in a <code>.mmpd</code> dump); here you are looking at the map, not the traffic.
      </p>
    </Explainer>
  )
}
