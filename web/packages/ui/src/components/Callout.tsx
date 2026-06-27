import type { ReactNode } from 'react'
import { AlertTriangle, Info, XCircle, type LucideIcon } from 'lucide-react'

export type CalloutVariant = 'danger' | 'warning' | 'error' | 'info'

const variants: Record<CalloutVariant, { container: string; icon: string; Icon: LucideIcon }> = {
  // Safety / destructive action — brand red, the strongest emphasis.
  danger: { container: 'border-syn-red/40 bg-syn-red/5', icon: 'text-syn-red', Icon: AlertTriangle },
  // Caution / degraded state the user should notice but that is not an error.
  warning: {
    container: 'border-status-warn/40 bg-status-warn/5',
    icon: 'text-status-warn',
    Icon: AlertTriangle,
  },
  // A failure that already happened (request error, invalid input).
  error: { container: 'border-status-bad/40 bg-status-bad/5', icon: 'text-status-bad', Icon: XCircle },
  // Neutral information.
  info: {
    container: 'border-status-info/40 bg-status-info/5',
    icon: 'text-status-info',
    Icon: Info,
  },
}

/**
 * A bordered callout for warnings, errors, and notices, shared across apps.
 *
 * The icon is aligned to the top of the text (`items-start`) and nudged down 2px (`mt-0.5`) to
 * sit on the first line's cap.
 */
export function Callout({
  variant = 'info',
  icon = true,
  className = '',
  children,
}: {
  variant?: CalloutVariant
  icon?: boolean
  className?: string
  children: ReactNode
}) {
  const { container, icon: iconCls, Icon } = variants[variant]
  return (
    <div
      className={`flex items-start gap-2 border ${container} px-4 py-3 text-xs leading-5 text-grey-700 ${className}`}
    >
      {icon && <Icon className={`h-4 w-4 shrink-0 mt-0.5 ${iconCls}`} aria-hidden />}
      <div className="min-w-0">{children}</div>
    </div>
  )
}
