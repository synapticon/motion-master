import Explainer from './Explainer'

// The "What is an ESI file?" teaching panel for the Tools ESI page. Kept as its own component so
// the explanation lives in one place if a device-scoped ESI view is ever added, matching how
// SiiExplainer is shared between the device and Tools SII pages.
export default function EsiExplainer() {
  return (
    <Explainer title="What is an ESI file?">
      <p>
        <strong>ESI</strong> stands for <strong>EtherCAT Slave Information</strong>. It is the XML
        device description a vendor ships alongside a device — the file a configuration tool reads
        to know what the device is, how its process data is laid out, and what its{' '}
        <strong>CoE object dictionary</strong> contains. Its format is specified in{' '}
        <strong>ETG.2000</strong>.
      </p>
      <p>
        <strong>Why not just read the device?</strong> You can enumerate a drive's object dictionary
        over the bus with the CoE SDO-Information service, and the{' '}
        <strong>Device → Parameters</strong> page does exactly that. But that service returns only
        the bare bones: index, subindex, data type, bit length and access. It carries{' '}
        <em>no descriptions, no enum option labels, no engineering units and no min/max bounds</em>.
        All of that exists only in the ESI — which is why this page exists, and why it needs no
        hardware.
      </p>
      <p>
        <strong>One file, many devices.</strong> An ESI describes a whole product family, so it
        usually holds several <code>&lt;Device&gt;</code> entries. Pick one to expand it.
      </p>
      <p>
        <strong>The dictionary is assembled, not just read.</strong> A modular device splits its
        objects across a device description and one or more <strong>modules</strong> plugged into{' '}
        <strong>slots</strong>. On a SOMANET drive the device itself declares only the
        communication objects (<code>0x1xxx</code>) — every CiA 402 object (<code>0x6040</code>{' '}
        Controlword, <code>0x6060</code> Modes of operation, <code>0x607A</code> Target position, …)
        lives in a module. The table below merges them, and the <strong>Source</strong> column
        records where each entry came from.
      </p>
      <p>
        <strong>When a slot offers a choice.</strong> Some slots list several mutually exclusive
        modules — a safe-motion drive may offer four FSoE variants, of which exactly one is
        physically fitted. Offline there is no way to know which, so every variant is merged and any
        overlap is reported below the table. Use <strong>Modules</strong> to narrow the merge to one
        real configuration.
      </p>
      <p>
        <strong>Values are shown as the file writes them.</strong> Default, minimum and maximum are
        raw <code>hexBinary</code>, <strong>least-significant byte first</strong> — so{' '}
        <code>92010200</code> is the 32-bit value <code>0x00020192</code>. That is the ESI's own
        spelling, kept verbatim so a value can be compared against the source file by eye.
      </p>
    </Explainer>
  )
}
