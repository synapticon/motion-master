import PageHeader from '../components/PageHeader'

export default function DashboardPage() {
  return (
    <div>
      <PageHeader eyebrow="Network" title="Devices" />
      <div className="p-8">
        <p className="text-grey-600 text-sm">
          No devices discovered. Start a network scan to detect EtherCAT slaves.
        </p>
      </div>
    </div>
  )
}
