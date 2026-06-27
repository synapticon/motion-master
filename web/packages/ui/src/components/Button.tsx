import type { ButtonHTMLAttributes } from 'react'

// Synapticon-red outline button: brand border + text, fills on hover, dimmed and
// non-interactive when disabled. The canonical action-button look across apps.
export const btnOutline =
  'border border-syn-red text-syn-red px-3 py-1.5 text-xs hover:bg-syn-red hover:text-white disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

/** A `<button>` pre-styled with {@link btnOutline}; all native button props pass through. */
export function Button({ className = '', ...props }: ButtonHTMLAttributes<HTMLButtonElement>) {
  return <button className={`${btnOutline} ${className}`} {...props} />
}
