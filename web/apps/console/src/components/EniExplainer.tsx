import Explainer from './Explainer'

// The "What is an ENI file?" teaching panel for the Tools ENI page. Its own component, matching how
// EsiExplainer and SiiExplainer are kept, so the explanation has one home if a second ENI view is
// ever added.
export default function EniExplainer() {
  return (
    <Explainer title="What is an ENI file?">
      <p>
        <strong>ENI</strong> stands for <strong>EtherCAT Network Information</strong>. Where an{' '}
        <strong>ESI</strong> file describes one device family, an ENI describes one{' '}
        <em>assembled network</em> — this bus, with these devices, mapped this way. It is what a
        third-party master reads to bring the same bus up: <strong>acontis EC-Master</strong>,{' '}
        <strong>TwinCAT</strong>, <strong>CODESYS</strong>. Its format is specified in{' '}
        <strong>ETG.2100</strong>, and what this page writes conforms to ENI Schema 1.7.
      </p>
      <p>
        <strong>An ENI is a script, not a description.</strong> This is the part that surprises
        people. An ESI is declarative, and a configuration tool reads it to decide what to do. An
        ENI is imperative: every configuration step is written out as an{' '}
        <strong>EtherCAT datagram</strong> or a <strong>CoE download</strong>, tagged with the
        AL-state transition it belongs to, and a master brings the bus up by replaying them in
        order. So the file holds the sync-manager and FMMU register writes byte for byte, the PDO
        assignment as SDO downloads, and the state walk — including the read the master repeats
        until each device has arrived, because an ENI has no wait instruction of its own.
      </p>
      <p>
        <strong>Why export from a live bus?</strong> The usual way to get an ENI is to collect every
        vendor's ESI files into a configuration tool and describe the network by hand. This does it
        the other way round: the bus in front of you already <em>is</em> the configuration, so it is
        read rather than described. Nothing has to be typed twice, and nothing can disagree with the
        hardware.
      </p>
      <p>
        <strong>The bus has to be mapped.</strong> FMMUs and logical addresses come into being at
        the <strong>SAFE-OP</strong> transition. Before that there is no mapping to describe, so a
        bus in PRE-OP is refused rather than guessed at. Use{' '}
        <strong>Fieldbus → Control</strong> to reach SAFE-OP or OP first.
      </p>
      <p>
        <strong>Exporting drives the bus.</strong> Each device's SII is read for its port layout and
        bootstrap mailbox, and its PDO assignment is read over CoE — one EEPROM read and a short
        burst of SDO uploads per device. It is a deliberate action, not a page that refreshes
        itself.
      </p>
      <p>
        <strong>The result runs in free-run.</strong> Distributed clocks are left out, which is how
        Motion Master runs the bus itself: devices act on frame arrival rather than on a SYNC0
        pulse. A synchronised configuration needs the <code>DC</code> element, and that needs an{' '}
        <code>AssignActivate</code> word which a SOMANET drive does not carry in its SII — it would
        have to come from the device's ESI.
      </p>
      <p>
        <strong>A warning costs an element, never the document.</strong> If a device's SII will not
        read, the export loses its port layout and its bootstrap mailbox and keeps every init
        command, so the file still brings the bus up. The count is reported beside the button and
        each warning is written to the server log.
      </p>
    </Explainer>
  )
}
