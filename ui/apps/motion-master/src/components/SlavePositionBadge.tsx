interface SlavePositionBadgeProps {
  position: number
}

/**
 * The slave-position chip used wherever a device's 1-based EtherCAT bus
 * position is shown (sidebar, Configuration, Process Image, Diagnostics, DC
 * Sync). Slate background + faint white border, padded to two digits so the
 * styling reads identically across every view.
 */
export default function SlavePositionBadge({ position }: SlavePositionBadgeProps) {
  return (
    <span
      className="inline-flex shrink-0 cursor-help items-center justify-center rounded-sm border border-white/25 bg-slate-200 px-1.5 py-0.5 font-mono font-semibold leading-none text-grey-900"
      title="Slave position — the device’s 1-based position on the EtherCAT bus, used in API endpoint paths, e.g. /api/devices/{slavePosition}/parameters"
    >
      {String(position).padStart(2, '0')}
    </span>
  )
}
