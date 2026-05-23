import PageHeader from '../components/PageHeader'

export default function DashboardPage() {
  return (
    <div>
      <PageHeader eyebrow="App" title="Dashboard" />
      <div className="p-8 space-y-4">
        <p className="text-grey-600 text-sm">
          No devices discovered. Start a network scan to detect EtherCAT slaves.
        </p>
        <p className="text-sm">
          <a
            href="/docs"
            target="_blank"
            rel="noopener noreferrer"
            className="text-blue-400 hover:text-blue-300 underline"
          >
            Documentation
          </a>
        </p>
      </div>
    </div>
  )
}
