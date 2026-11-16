interface MailboxCapabilitiesProps {
  /** The CoE features the server named from the CoE details byte. */
  coeDetailNames: string[];
  foeDetails: number;
  eoeDetails: number;
}

// The full name of each mailbox protocol that the server names, for a tooltip.
export const MAILBOX_PROTOCOL_DESCRIPTIONS: Record<string, string> = {
  AoE: 'ADS over EtherCAT',
  EoE: 'Ethernet over EtherCAT',
  CoE: 'CAN application protocol over EtherCAT',
  FoE: 'File Access over EtherCAT',
  SoE: 'Servo Drive Profile over EtherCAT',
  VoE: 'Vendor specific protocol over EtherCAT',
};

// The CoE features that a device can advertise, in the order of their bits, with what each means.
// The server names the ones a device advertises; every one is shown, lit when it is advertised.
// FoE and EoE carry a single "enabled" bit each.
const COE_FEATURES: { label: string; desc: string }[] = [
  { label: 'SDO', desc: 'SDO object access (mailbox reads/writes).' },
  { label: 'SDO Info', desc: 'SDO Information service — object-dictionary enumeration.' },
  { label: 'PDO Assign', desc: 'PDO assignment configurable (0x1C1x).' },
  { label: 'PDO Config', desc: 'PDO mapping configurable (0x16xx/0x1Axx).' },
  {
    label: 'Upload',
    desc: 'Upload at startup — the master reads the device’s PDO configuration from the slave at startup rather than trusting the static ESI mapping.',
  },
  {
    label: 'Complete Access',
    desc: 'SDO Complete Access. Advertised only — SOMANET drives support Complete Access even when this reads off, so Motion Master probes actual support at runtime rather than trusting this bit.',
  },
];

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
  );
}

// The advertised CoE, FoE and EoE mailbox capabilities as labelled chips. Shared by the
// Configuration page (bus-config) and the SII page (the General category of the EEPROM), so both
// render the same.
export default function MailboxCapabilities({
  coeDetailNames,
  foeDetails,
  eoeDetails,
}: MailboxCapabilitiesProps) {
  return (
    <div className="flex flex-wrap gap-1.5">
      {COE_FEATURES.map(({ label, desc }) => (
        <Chip key={label} on={coeDetailNames.includes(label)} label={`CoE ${label}`} desc={desc} />
      ))}
      <Chip
        on={(foeDetails & 0x01) !== 0}
        label="FoE"
        desc="File over EtherCAT (firmware transfer) enabled."
      />
      <Chip on={(eoeDetails & 0x01) !== 0} label="EoE" desc="Ethernet over EtherCAT enabled." />
    </div>
  );
}
