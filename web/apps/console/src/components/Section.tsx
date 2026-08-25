import type { ReactNode } from 'react'

/**
 * The one container a device-page panel sits in.
 *
 * It exists because the Safety page had grown three different section
 * treatments at once - a bordered `section` with a display-font heading, a
 * bordered `div` with an uppercase span, and an unbordered block with no
 * boundary at all. Three boundaries mean no boundary: the eye cannot tell where
 * one subject stops and the next begins, which is what makes a long page read
 * as a wall rather than as a list of separable things.
 *
 * `chips` are indicators that belong on the heading line because they qualify
 * the whole section - a connection state, a monitoring mode. `description` is
 * the sentence explaining what the section is for; omit it when the title
 * already says everything. `actions` sit at the right of the heading and are
 * for controls that act on the section as a whole.
 */
export default function Section({
  title,
  chips,
  description,
  actions,
  children,
}: {
  title: string
  chips?: ReactNode
  description?: ReactNode
  actions?: ReactNode
  children: ReactNode
}) {
  return (
    <section className="border border-grey-200">
      <header className="px-4 py-3 border-b border-grey-200">
        <div className="flex items-center gap-2 flex-wrap">
          <h2 className="text-xs uppercase tracking-wider text-grey-700">{title}</h2>
          {chips}
          {actions && <div className="ml-auto flex items-center gap-2">{actions}</div>}
        </div>
        {description && <p className="mt-1 text-xs text-grey-600 max-w-4xl">{description}</p>}
      </header>
      {children}
    </section>
  )
}
