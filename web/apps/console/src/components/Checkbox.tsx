import { useEffect, useRef } from 'react'

interface CheckboxProps {
  checked: boolean
  onChange: (checked: boolean) => void
  /**
   * Renders the dash state: some but not all of what this controls is checked. Only meaningful on
   * a "select all" checkbox. `checked` is ignored visually while this is set, matching the native
   * behaviour, so pass `checked={allSelected}` and let this override it.
   */
  indeterminate?: boolean
  disabled?: boolean
  title?: string
  'aria-label'?: string
  /** Extra classes. The accent, cursor and disabled treatment are supplied and should not be
      restated at the call site — that is the point of this component. */
  className?: string
}

// The one checkbox in the app. It exists because there were four hand-rolled ones that disagreed:
// three rendered in the browser's default blue and one in Synapticon red, and none of them could
// show the indeterminate state a "select all" header needs.
//
// `indeterminate` is a DOM property, not an HTML attribute — React cannot set it through JSX — so
// it has to be written onto the node through a ref.
export default function Checkbox({
  checked,
  onChange,
  indeterminate = false,
  disabled,
  title,
  'aria-label': ariaLabel,
  className = '',
}: CheckboxProps) {
  const ref = useRef<HTMLInputElement>(null)

  useEffect(() => {
    if (ref.current) {
      ref.current.indeterminate = indeterminate
    }
  }, [indeterminate])

  return (
    <input
      ref={ref}
      type="checkbox"
      checked={checked}
      onChange={e => onChange(e.target.checked)}
      disabled={disabled}
      title={title}
      aria-label={ariaLabel}
      className={`accent-syn-red cursor-pointer disabled:cursor-not-allowed disabled:opacity-50 ${className}`}
    />
  )
}

/**
 * State of a "select all" checkbox over @p selected of @p total items, plus what a click should do.
 *
 * The convention is the one Gmail and GitHub use, and the reason this is a function rather than
 * three expressions at each call site: **any selection clears; no selection selects all.** So a
 * partially-filled box empties on the first click rather than filling — which is what someone who
 * has ticked three of forty rows and wants to start over expects.
 */
export function selectAllState(selected: number, total: number) {
  return {
    checked: total > 0 && selected === total,
    indeterminate: selected > 0 && selected < total,
    /** True when a click should select everything; false when it should clear. */
    selectsAll: selected === 0,
  }
}
