import type { ReactNode } from 'react'

interface PageHeaderProps {
  eyebrow: string
  title: string
  description?: ReactNode
}

export default function PageHeader({ eyebrow, title, description }: PageHeaderProps) {
  return (
    <div className="px-8 py-7 border-b border-grey-200">
      <p className="eyebrow mb-2">{eyebrow}</p>
      <h1 className="font-display text-4xl font-light">{title}</h1>
      {description && <p className="text-sm text-grey-600 mt-2">{description}</p>}
    </div>
  )
}
