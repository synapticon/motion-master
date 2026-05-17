import { NavLink, Outlet, useParams } from 'react-router'

const deviceLinks = [
  { to: 'ethercat-state',    label: 'EtherCAT State' },
  { to: 'object-dictionary', label: 'Object Dictionary' },
  { to: 'sii',               label: 'SII' },
  { to: 'registers',         label: 'Registers' },
  { to: 'foe',               label: 'FoE' },
  { to: 'process-data',      label: 'Process Data' },
]

function NavItem({ to, label }: { to: string; label: string }) {
  return (
    <NavLink
      to={to}
      className={({ isActive }) =>
        `block px-5 py-2 text-xs font-display uppercase tracking-widest border-l-2 transition-colors ${
          isActive
            ? 'border-syn-red text-white bg-white/10'
            : 'border-transparent text-white/60 hover:text-white hover:border-white/30'
        }`
      }
    >
      {label}
    </NavLink>
  )
}

function DeviceSection({ deviceId }: { deviceId: string }) {
  return (
    <div className="mt-6">
      <p className="eyebrow px-5 mb-2">Device {deviceId}</p>
      {deviceLinks.map(({ to, label }) => (
        <NavItem key={to} to={`/devices/${deviceId}/${to}`} label={label} />
      ))}
    </div>
  )
}

export default function RootLayout() {
  const { deviceId } = useParams()

  return (
    <div className="flex h-screen bg-grey-50 text-grey-900">
      {/* Sidebar — Ocean Dark */}
      <aside className="w-56 shrink-0 bg-ocean-dark flex flex-col">
        <div className="px-5 py-4 border-b border-white/10">
          <span className="font-display text-sm font-medium uppercase tracking-widest text-white">
            Motion Master
          </span>
        </div>

        <nav className="flex-1 overflow-y-auto py-4">
          <NavItem to="/" label="Dashboard" />
          <DeviceSection deviceId={deviceId ?? '1'} />
        </nav>

        <div className="px-5 py-3 border-t border-white/10">
          <span className="text-white/30 text-xs font-display uppercase tracking-wider">
            v6.0.0
          </span>
        </div>
      </aside>

      {/* Content */}
      <main className="flex-1 overflow-auto bg-white">
        <Outlet />
      </main>
    </div>
  )
}
