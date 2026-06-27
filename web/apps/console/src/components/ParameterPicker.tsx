import { useEffect, useId, useMemo, useRef, useState } from 'react'
import { Check, ChevronDown, Search, X } from 'lucide-react'
import { type DeviceParameter, formatHex } from '@synapticon/motion-master-client'

interface ParameterPickerProps {
  /** The device's enumerated object dictionary. May be empty while a scan is in flight. */
  params: DeviceParameter[]
  /** Currently selected object index, or null when nothing is picked. */
  selectedIndex: number | null
  /** Currently selected object subindex, or null when nothing is picked. */
  selectedSubindex: number | null
  onSelect: (param: DeviceParameter) => void
  disabled?: boolean
  /** Shown on the trigger button when no parameter is selected. */
  placeholder?: string
  /**
   * Predicate for entries that should be shown but not selectable — rendered greyed, skipped by
   * keyboard navigation, and ignored on click. Used to keep meaningless picks (e.g. PDO mapping
   * objects) visible-but-inert rather than silently hidden.
   */
  isDisabled?: (param: DeviceParameter) => boolean
  /** Right-aligned tag + hover title shown on rows flagged by `isDisabled`. */
  disabledHint?: string
  /** Width utility for the trigger; the dropdown sizes itself to match. Defaults to `w-[32rem]`. */
  className?: string
}

// The display label for one object dictionary entry: "0x6064:00 — Position actual value".
// Stable across the picker (trigger, list, search haystack) so the formats never drift.
function paramLabel(p: DeviceParameter): string {
  return `${formatHex(p.index, 4)}:${formatHex(p.subindex, 2, false)} — ${p.name}`
}

// The lowercased haystack a query is matched against. Folds in both the rendered hex form
// ("0x6064:00") and the bare decimal index/subindex, so a user can search by name, by hex, by
// decimal, or by the "index:subindex" pair without thinking about which the device reports in.
function haystack(p: DeviceParameter): string {
  return `${formatHex(p.index, 4)}:${formatHex(p.subindex, 2, false)} ${p.name} ${p.index} ${p.subindex}`.toLowerCase()
}

// Every whitespace-separated token of the query must appear in the haystack (AND match), so
// "6064 position" narrows to the one entry matching both, regardless of token order.
function matches(p: DeviceParameter, query: string): boolean {
  const q = query.trim().toLowerCase()
  if (q === '') return true
  const hay = haystack(p)
  return q.split(/\s+/).every((tok) => hay.includes(tok))
}

/**
 * A searchable combobox for picking a device parameter (CoE object) from an object dictionary that
 * routinely runs to hundreds of entries. Users can scroll the full list, or filter by name, by hex
 * or decimal index/subindex, or by the "index:subindex" pair. Keyboard-driven: ↑/↓ move, Enter
 * picks, Esc closes. Reusable anywhere a parameter must be chosen (monitorings, and beyond).
 */
export default function ParameterPicker({
  params,
  selectedIndex,
  selectedSubindex,
  onSelect,
  disabled = false,
  placeholder = 'Search parameters…',
  isDisabled,
  disabledHint,
  className = 'w-[32rem]',
}: ParameterPickerProps) {
  const [open, setOpen] = useState(false)
  const [query, setQuery] = useState('')
  const [highlight, setHighlight] = useState(0)
  const rootRef = useRef<HTMLDivElement>(null)
  const inputRef = useRef<HTMLInputElement>(null)
  const listRef = useRef<HTMLUListElement>(null)
  const listId = useId()

  const selected = useMemo(
    () =>
      selectedIndex !== null && selectedSubindex !== null
        ? (params.find((p) => p.index === selectedIndex && p.subindex === selectedSubindex) ?? null)
        : null,
    [params, selectedIndex, selectedSubindex],
  )

  const filtered = useMemo(() => params.filter((p) => matches(p, query)), [params, query])
  // Per-row selectability, aligned with `filtered`. Disabled rows stay visible but inert.
  const disabledFlags = useMemo(
    () => filtered.map((p) => isDisabled?.(p) ?? false),
    [filtered, isDisabled],
  )

  // The next selectable row at or after `start` in direction `dir` (+1/−1), or −1 if none.
  function findEnabled(start: number, dir: number): number {
    for (let i = start; i >= 0 && i < filtered.length; i += dir) {
      if (!disabledFlags[i]) return i
    }
    return -1
  }

  // Close on an outside click so the dropdown behaves like a native control.
  useEffect(() => {
    if (!open) return
    function onDown(e: MouseEvent) {
      if (rootRef.current && !rootRef.current.contains(e.target as Node)) setOpen(false)
    }
    document.addEventListener('mousedown', onDown)
    return () => document.removeEventListener('mousedown', onDown)
  }, [open])

  // On open, focus the search box and clear any prior query.
  useEffect(() => {
    if (open) {
      setQuery('')
      // Focus after the dropdown mounts.
      requestAnimationFrame(() => inputRef.current?.focus())
    }
  }, [open])

  // As the filter changes (and on open), park the highlight on the first selectable row.
  useEffect(() => {
    const first = findEnabled(0, 1)
    setHighlight(first === -1 ? 0 : first)
    // findEnabled reads filtered/disabledFlags; re-run when those change.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [filtered, disabledFlags])

  useEffect(() => {
    if (!open) return
    const el = listRef.current?.children[highlight] as HTMLElement | undefined
    el?.scrollIntoView({ block: 'nearest' })
  }, [highlight, open])

  function choose(p: DeviceParameter) {
    onSelect(p)
    setOpen(false)
  }

  function onKeyDown(e: React.KeyboardEvent) {
    if (e.key === 'ArrowDown') {
      e.preventDefault()
      const next = findEnabled(highlight + 1, 1)
      if (next !== -1) setHighlight(next)
    } else if (e.key === 'ArrowUp') {
      e.preventDefault()
      const prev = findEnabled(highlight - 1, -1)
      if (prev !== -1) setHighlight(prev)
    } else if (e.key === 'Enter') {
      e.preventDefault()
      const p = filtered[highlight]
      if (p && !disabledFlags[highlight]) choose(p)
    } else if (e.key === 'Escape') {
      e.preventDefault()
      setOpen(false)
    }
  }

  return (
    <div ref={rootRef} className={`relative ${className}`}>
      <button
        type="button"
        disabled={disabled}
        onClick={() => setOpen((o) => !o)}
        className="flex w-full items-center justify-between gap-2 border border-grey-300 bg-white px-3 py-2 text-left font-sans text-sm normal-case tracking-normal disabled:cursor-not-allowed disabled:opacity-50"
        title={selected ? paramLabel(selected) : undefined}
      >
        <span className={`truncate ${selected ? 'text-grey-900' : 'text-grey-400'}`}>
          {selected ? paramLabel(selected) : placeholder}
        </span>
        <ChevronDown className="h-4 w-4 shrink-0 text-grey-400" />
      </button>

      {open && (
        <div className="absolute z-20 mt-1 w-full border border-grey-300 bg-white shadow-lg">
          <div className="flex items-center gap-2 border-b border-grey-200 px-2">
            <Search className="h-4 w-4 shrink-0 text-grey-400" />
            <input
              ref={inputRef}
              value={query}
              onChange={(e) => setQuery(e.target.value)}
              onKeyDown={onKeyDown}
              placeholder="Name, index, or index:subindex…"
              className="w-full bg-transparent py-2 text-sm outline-none"
              role="combobox"
              aria-expanded
              aria-controls={listId}
              aria-autocomplete="list"
            />
            {query && (
              <button
                type="button"
                onClick={() => {
                  setQuery('')
                  inputRef.current?.focus()
                }}
                className="shrink-0 text-grey-400 hover:text-grey-700"
                title="Clear search"
              >
                <X className="h-4 w-4" />
              </button>
            )}
          </div>

          <ul ref={listRef} id={listId} role="listbox" className="max-h-72 overflow-y-auto">
            {filtered.length === 0 ? (
              <li className="px-3 py-3 text-sm text-grey-400">
                {params.length === 0 ? 'No parameters available' : 'No matches'}
              </li>
            ) : (
              filtered.map((p, i) => {
                const isSelected =
                  p.index === selectedIndex && p.subindex === selectedSubindex
                const rowDisabled = disabledFlags[i]
                const isHighlighted = i === highlight && !rowDisabled
                return (
                  <li
                    key={`${p.index}:${p.subindex}`}
                    role="option"
                    aria-selected={isSelected}
                    aria-disabled={rowDisabled}
                    onClick={() => {
                      if (!rowDisabled) choose(p)
                    }}
                    onMouseEnter={() => {
                      if (!rowDisabled) setHighlight(i)
                    }}
                    title={rowDisabled ? disabledHint : undefined}
                    className={`flex items-center gap-2 px-3 py-1.5 text-sm ${
                      rowDisabled
                        ? 'cursor-not-allowed text-grey-300'
                        : `cursor-pointer ${isHighlighted ? 'bg-ocean text-white' : 'text-grey-900'}`
                    }`}
                  >
                    <span
                      className={`shrink-0 font-mono text-xs ${
                        rowDisabled
                          ? 'text-grey-300'
                          : isHighlighted
                            ? 'text-white/80'
                            : 'text-grey-500'
                      }`}
                    >
                      {formatHex(p.index, 4)}:{formatHex(p.subindex, 2, false)}
                    </span>
                    <span className="truncate">{p.name}</span>
                    {rowDisabled && disabledHint ? (
                      <span className="ml-auto shrink-0 text-[10px] uppercase tracking-wide text-grey-400">
                        {disabledHint}
                      </span>
                    ) : (
                      isSelected && <Check className="ml-auto h-4 w-4 shrink-0" />
                    )}
                  </li>
                )
              })
            )}
          </ul>
        </div>
      )}
    </div>
  )
}
