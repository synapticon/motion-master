import { useEffect, useRef, useState } from 'react'
import type { ReactNode } from 'react'

// The section machinery every Learn page uses: a sticky bar of section names across the top, and
// the sections themselves underneath.
//
// One array drives both, so a page cannot end up with a link to a section that no longer exists or
// a section nothing links to. That is the whole reason this is a component rather than a pattern
// each page copies.
//
// Prose is capped at a readable measure by `[&>p]:max-w-3xl` rather than by a wrapper, so a figure
// or an interactive block dropped into the same section still spans the full width.

export interface LearnSectionEntry {
  /** URL fragment and scroll target. Kebab-case, stable — people link to these. */
  id: string
  /** The section heading. Free to be descriptive. */
  title: string
  /** Short label for the sticky bar. Falls back to the title when the title is already short. */
  tab?: string
  content: ReactNode
}

export default function LearnSections({ sections }: { sections: LearnSectionEntry[] }) {
  const [active, setActive] = useState(sections[0]?.id ?? '')
  const visible = useRef(new Map<string, boolean>())
  const latest = useRef(sections)
  latest.current = sections
  const ids = sections.map(section => section.id).join('|')

  // Which section is "current" is a judgement, not a fact: several are on screen at once. The
  // bottom margin discounts everything below the top third of the viewport, so the answer is the
  // first section whose heading has reached the reading area rather than whatever is merely
  // visible.
  useEffect(() => {
    const elements = latest.current
      .map(section => document.getElementById(section.id))
      .filter((element): element is HTMLElement => element !== null)
    if (elements.length === 0) {
      return
    }
    const observer = new IntersectionObserver(
      entries => {
        for (const entry of entries) {
          visible.current.set(entry.target.id, entry.isIntersecting)
        }
        const current = latest.current.find(section => visible.current.get(section.id))
        if (current) {
          setActive(current.id)
        }
      },
      { rootMargin: '-64px 0px -66% 0px' },
    )
    elements.forEach(element => observer.observe(element))
    return () => observer.disconnect()
  }, [ids])

  // A link from another Learn page arrives with a fragment, but the browser resolves it before
  // React has rendered any of these sections, so it finds nothing and stays at the top. Do the
  // scroll here instead, once, after the first render.
  useEffect(() => {
    const id = window.location.hash.slice(1)
    if (id && latest.current.some(section => section.id === id)) {
      document.getElementById(id)?.scrollIntoView({ block: 'start' })
      setActive(id)
    }
  }, [])

  const goTo = (id: string) => {
    setActive(id)
    document.getElementById(id)?.scrollIntoView({ behavior: 'smooth', block: 'start' })
    // Leaves a shareable URL without adding a history entry per click.
    window.history.replaceState(null, '', `#${id}`)
  }

  return (
    <div>
      <nav
        aria-label="Sections on this page"
        className="sticky top-0 z-10 bg-white border-b border-grey-200"
      >
        <div className="flex overflow-x-auto px-4 sm:px-8 [scrollbar-width:none] [&::-webkit-scrollbar]:hidden">
          {sections.map(section => (
            <button
              key={section.id}
              type="button"
              onClick={() => goTo(section.id)}
              aria-current={active === section.id ? 'true' : undefined}
              className={`shrink-0 px-3 first:pl-0 py-3 text-xs border-b-2 -mb-px transition-colors cursor-pointer ${
                active === section.id
                  ? 'border-syn-red text-syn-red'
                  : 'border-transparent text-grey-600 hover:text-grey-900'
              }`}
            >
              {section.tab ?? section.title}
            </button>
          ))}
        </div>
      </nav>

      <div className="p-4 sm:px-8 sm:py-7 space-y-8">
        {sections.map(section => (
          <section key={section.id} id={section.id} className="space-y-3 scroll-mt-14">
            <h2 className="text-sm text-grey-900">{section.title}</h2>
            <div className="space-y-3 text-sm text-grey-700 leading-relaxed [&>p]:max-w-3xl">
              {section.content}
            </div>
          </section>
        ))}
      </div>
    </div>
  )
}
