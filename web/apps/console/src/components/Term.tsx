// A glossary term: the word stays in the sentence, and its definition sits one hover or tap away.
//
// Two people read a Learn page at once. A control engineer knows what a following error is and
// should not have to read past a definition of it; someone arriving from software has never met
// the phrase. Marking the word up serves both, because the prose never stops to explain itself.
//
// Hover alone is not enough, since a touch screen has none, so the term is a button that opens on
// tap and on keyboard focus as well. It closes on Escape, on blur, and on a pointer press
// anywhere else.

import { useEffect, useId, useLayoutEffect, useRef, useState } from 'react'
import type { ReactNode } from 'react'
import { glossary, type GlossaryId } from './glossary'

/** Matches `w-80` below. Used to decide which way the popover opens, before it is rendered. */
const popoverWidth = 320

export default function Term({ id, children }: { id: GlossaryId; children?: ReactNode }) {
  const entry = glossary[id]
  const [open, setOpen] = useState(false)
  const [alignRight, setAlignRight] = useState(false)
  const wrapper = useRef<HTMLSpanElement>(null)
  const popoverId = useId()

  useEffect(() => {
    if (!open) {
      return
    }
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        setOpen(false)
      }
    }
    const onPointerDown = (event: PointerEvent) => {
      if (!wrapper.current?.contains(event.target as Node)) {
        setOpen(false)
      }
    }
    document.addEventListener('keydown', onKeyDown)
    document.addEventListener('pointerdown', onPointerDown)
    return () => {
      document.removeEventListener('keydown', onKeyDown)
      document.removeEventListener('pointerdown', onPointerDown)
    }
  }, [open])

  // A term near the right edge would open a fixed-width popover off the screen. Decide from the
  // term's own position rather than the popover's, so the answer does not depend on the alignment
  // it is about to set.
  useLayoutEffect(() => {
    if (!open || !wrapper.current) {
      return
    }
    const rect = wrapper.current.getBoundingClientRect()
    setAlignRight(rect.left + popoverWidth > window.innerWidth - 8)
  }, [open])

  return (
    <span ref={wrapper} className="relative">
      <button
        type="button"
        aria-expanded={open}
        aria-describedby={open ? popoverId : undefined}
        onClick={() => setOpen(true)}
        onFocus={() => setOpen(true)}
        onBlur={() => setOpen(false)}
        onPointerEnter={event => {
          if (event.pointerType === 'mouse') {
            setOpen(true)
          }
        }}
        onPointerLeave={event => {
          if (event.pointerType === 'mouse') {
            setOpen(false)
          }
        }}
        className="border-b border-dotted border-grey-500 text-grey-900 hover:border-syn-red hover:text-syn-red transition-colors cursor-help"
      >
        {children ?? entry.term}
      </button>
      {open && (
        <span
          id={popoverId}
          role="tooltip"
          className={`absolute top-full z-20 mt-1 block w-80 border border-grey-300 bg-white px-3 py-2 text-xs leading-5 text-grey-700 shadow-lg ${
            alignRight ? 'right-0' : 'left-0'
          }`}
        >
          <span className="block font-display uppercase tracking-wide text-[10px] text-grey-600">
            {entry.term}
          </span>
          <span className="block mt-1">{entry.definition}</span>
        </span>
      )}
    </span>
  )
}
