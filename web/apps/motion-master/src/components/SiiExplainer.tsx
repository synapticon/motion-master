import Explainer from './Explainer'

// The "What is SII?" teaching panel, shared by the device SII page and the Tools SII page so the
// explanation stays in one place.
export default function SiiExplainer() {
  return (
    <Explainer title="What is SII?">
      <p>
        <strong>SII</strong> stands for <strong>Slave Information Interface</strong>. It is the
        standardised read interface to the <strong>EEPROM</strong> — a small non-volatile memory
        chip on every EtherCAT slave, wired directly to the ESC (EtherCAT Slave Controller). The
        EtherCAT standard (ETG.1000) calls the data the “SII”; in practice people use “SII” and
        “EEPROM” interchangeably.
      </p>
      <p>
        It holds the slave's <strong>identity and self-description</strong>: vendor ID, product
        code, revision and serial number, the configured mailbox sizes and offsets, the default
        Sync Manager and FMMU configuration, the device / group / order name strings, and the
        default PDO mappings. At power-on the ESC automatically loads the first part of it to
        configure itself, before the master ever communicates with the device.
      </p>
      <p>
        <strong>Is it the same as registers?</strong> No — the SII is a separate EEPROM chip, not
        ESC registers. But you reach it <em>through</em> registers: the ESC exposes an
        EEPROM-control window (configuration <code>0x0500</code>, control/status <code>0x0502</code>,
        address <code>0x0504</code>, data <code>0x0508</code>). So “SII” is the <em>logical
        layout</em> of the data, and the “EEPROM registers” are the <em>transport</em>. The SII is
        addressed in 16-bit words, not bytes.
      </p>
      <p>
        <strong>How is it read?</strong> The master performs a small handshake on those control
        registers to fetch the EEPROM one word at a time, looping over the word addresses to read
        the whole image. This is a control-plane operation done off the real-time loop, and it is
        most reliable while the device is in the <strong>INIT</strong> or <strong>PRE-OP</strong>{' '}
        state.
      </p>
      <p>
        Structurally the image is a fixed <strong>128-byte header</strong> (the identity and mailbox
        fields) followed by a sequence of variable-length <strong>categories</strong> — strings,
        general info, FMMU and Sync-Manager defaults, default PDO mappings, distributed-clock
        settings — each tagged with a type and a length, walked in order until an end marker. That
        is the structure decoded and displayed below.
      </p>
    </Explainer>
  )
}
