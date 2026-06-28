interface SlavePositionBadgeProps {
  position: number
  /**
   * `solid` (default) — grey fill, for the light-background pages (Configuration,
   * Process Image, Diagnostics, DC Sync) where it should read as a distinct chip.
   * `muted` — transparent fill for dark surfaces (the sidebar), where a grey chip
   * would over-dominate; the position is a secondary detail there, not the headline.
   */
  tone?: 'solid' | 'muted'
}

/**
 * The slave-position chip used wherever a device's 1-based EtherCAT bus
 * position is shown (sidebar, Configuration, Process Image, Diagnostics, DC
 * Sync). Padded to two digits so the styling reads identically across views.
 */
export default function SlavePositionBadge({ position, tone = 'solid' }: SlavePositionBadgeProps) {
  const toneClasses =
    tone === 'muted'
      ? 'border-white/20 bg-white/10 text-white/60'
      : 'border-white/25 bg-grey-200 text-grey-900'
  return (
    <span
      className={`inline-flex shrink-0 cursor-help items-center justify-center rounded-sm border px-1.5 py-0.5 font-mono font-semibold leading-none ${toneClasses}`}
      title="Slave position — the device’s 1-based position on the EtherCAT bus, used in API endpoint paths, e.g. /api/devices/{slavePosition}/parameters"
    >
      {String(position).padStart(2, '0')}
    </span>
  )
}
