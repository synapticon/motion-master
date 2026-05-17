export default function DashboardPage() {
  return (
    <div className="p-8">
      <p className="eyebrow mb-2">Network</p>
      <h1 className="font-display text-4xl font-light mb-8">Devices</h1>
      <p className="text-grey-600 text-sm">
        No devices discovered. Start a network scan to detect EtherCAT slaves.
      </p>
    </div>
  )
}
