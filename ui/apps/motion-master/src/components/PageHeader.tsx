interface PageHeaderProps {
  eyebrow: string
  title: string
}

export default function PageHeader({ eyebrow, title }: PageHeaderProps) {
  return (
    <div className="px-8 py-7 border-b border-grey-200">
      <p className="eyebrow mb-2">{eyebrow}</p>
      <h1 className="font-display text-4xl font-light">{title}</h1>
    </div>
  )
}
