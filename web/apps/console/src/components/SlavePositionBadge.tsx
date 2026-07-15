interface SlavePositionBadgeProps {
  position: number
}

/**
 * The slave-position chip used wherever a device's 1-based EtherCAT bus position is
 * shown (sidebar, Configuration, Process Image, Diagnostics, DC Sync). A filled
 * orange chip (the `position` accent) with dark text, styled identically on light
 * pages and the dark sidebar alike so the identifier is instantly recognisable
 * everywhere. Padded to two digits so the width is stable across views.
 */
export default function SlavePositionBadge({ position }: SlavePositionBadgeProps) {
  return (
    <span
      // pt is 2px more than pb on purpose: with leading-none the monospace digits
      // sit high in the line box (the empty descent slack falls to the bottom), so a
      // touch of extra top padding optically centres them. Don't collapse to py-1.
      className="inline-flex shrink-0 cursor-help items-center justify-center rounded-sm border border-position bg-position px-2.5 pt-[6px] pb-1 font-mono text-sm font-semibold leading-none text-grey-900"
      title="Slave position — the device’s 1-based position on the EtherCAT bus, used in API endpoint paths, e.g. /api/devices/{slavePosition}/parameters"
    >
      {String(position).padStart(2, '0')}
    </span>
  )
}
