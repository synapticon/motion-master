interface MailboxCapabilitiesProps {
  coeDetails: number
  foeDetails: number
  eoeDetails: number
}

// CoE detail bits (ECT_COEDET_*), advertised in the device EEPROM. The rich flag set; FoE/EoE
// carry essentially a single "enabled" bit each (the supported-protocol set itself is the mailbox
// `protocols` bitfield, shown separately).
const COE_BITS: { bit: number; label: string; desc: string }[] = [
  { bit: 0x01, label: 'SDO', desc: 'SDO object access (mailbox reads/writes).' },
  {
    bit: 0x02,
    label: 'SDO Info',
    desc: 'SDO Information service — object-dictionary enumeration.',
  },
  { bit: 0x04, label: 'PDO Assign', desc: 'PDO assignment configurable (0x1C1x).' },
  { bit: 0x08, label: 'PDO Config', desc: 'PDO mapping configurable (0x16xx/0x1Axx).' },
  {
    bit: 0x10,
    label: 'Upload',
    desc: 'Upload at startup — the master reads the device’s PDO configuration from the slave at startup rather than trusting the static ESI mapping.',
  },
  {
    bit: 0x20,
    label: 'Complete Access',
    desc: 'SDO Complete Access. Advertised only — SOMANET drives support Complete Access even when this reads off, so Motion Master probes actual support at runtime rather than trusting this bit.',
  },
]

function Chip({ on, label, desc }: { on: boolean; label: string; desc: string }) {
  return (
    <span
      title={desc}
      className={`inline-flex items-center gap-1 rounded-sm border px-1.5 py-0.5 text-[10px] font-display uppercase tracking-wide cursor-help ${
        on ? 'border-status-good/40 text-status-good' : 'border-grey-200 text-grey-400'
      }`}
    >
      <span
        className={`inline-block w-1 h-1 rounded-full ${on ? 'bg-status-good' : 'bg-grey-300'}`}
      />
      {label}
    </span>
  )
}

// Decodes the advertised CoE/FoE/EoE mailbox detail bytes into labelled capability chips. Shared by
// the Configuration page (bytes from bus-config) and the SII page (bytes from the EEPROM General
// category), so both render identically from a single decode.
export default function MailboxCapabilities({
  coeDetails,
  foeDetails,
  eoeDetails,
}: MailboxCapabilitiesProps) {
  return (
    <div className="flex flex-wrap gap-1.5">
      {COE_BITS.map(({ bit, label, desc }) => (
        <Chip key={label} on={(coeDetails & bit) !== 0} label={`CoE ${label}`} desc={desc} />
      ))}
      <Chip
        on={(foeDetails & 0x01) !== 0}
        label="FoE"
        desc="File over EtherCAT (firmware transfer) enabled."
      />
      <Chip
        on={(eoeDetails & 0x01) !== 0}
        label="EoE"
        desc="Ethernet over EtherCAT enabled."
      />
    </div>
  )
}
