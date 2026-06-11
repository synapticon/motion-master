import { useState } from 'react'
import type { ReactNode } from 'react'
import { ChevronDown } from 'lucide-react'

interface ExplainerProps {
  /** Header label, e.g. "What is SII?". */
  title: string
  /** Explanatory body, revealed when the panel is expanded. */
  children: ReactNode
  /** Whether the panel starts expanded. Defaults to collapsed. */
  defaultOpen?: boolean
}

// A collapsible teaching panel: a clickable header (collapsed by default) over a block of
// explanatory prose. Used to make a page self-explanatory to someone new to the domain without
// crowding the working controls. Reused across pages (SII, Control, …) for a consistent "what is
// this?" affordance.
export default function Explainer({ title, children, defaultOpen = false }: ExplainerProps) {
  const [open, setOpen] = useState(defaultOpen)
  return (
    <section className="border border-grey-200">
      <button
        type="button"
        onClick={() => setOpen(o => !o)}
        aria-expanded={open}
        className="w-full flex items-center justify-between gap-3 px-4 py-3 text-left hover:bg-grey-50 transition-colors cursor-pointer"
      >
        <span className="eyebrow">{title}</span>
        <ChevronDown
          aria-hidden="true"
          className={`w-4 h-4 text-grey-500 shrink-0 transition-transform ${open ? 'rotate-180' : ''}`}
        />
      </button>
      {open && (
        <div className="border-t border-grey-200 px-4 py-4 text-sm text-grey-700 leading-relaxed space-y-3">
          {children}
        </div>
      )}
    </section>
  )
}
