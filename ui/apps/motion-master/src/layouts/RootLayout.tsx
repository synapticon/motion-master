import { NavLink, Outlet } from 'react-router'
import { useQuery } from '@tanstack/react-query'
import PwaUpdatePrompt from '../components/PwaUpdatePrompt'
import { useConnection } from '../contexts/ConnectionContext'
import { useApiHealth } from '../hooks/useApiHealth'

const deviceLinks = [
  { to: 'foe',        label: 'FoE' },
  { to: 'parameters', label: 'Parameters' },
  { to: 'registers',  label: 'Registers' },
  { to: 'sii',        label: 'SII' },
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
      <div className="pl-3">
        {deviceLinks.map(({ to, label }) => (
          <NavItem key={to} to={`/devices/${deviceId}/${to}`} label={label} />
        ))}
      </div>
    </div>
  )
}

export default function RootLayout() {
  const { api, hasScanned } = useConnection()
  const online = useApiHealth(api)

  const devicesQuery = useQuery({
    queryKey: ['devices'],
    queryFn: () => api.getDevices(),
    enabled: hasScanned,
  })

  const devices = devicesQuery.data?.data ?? []

  return (
    <div className="flex h-screen bg-grey-50 text-grey-900">
      {/* Sidebar — Ocean Dark */}
      <aside className="w-56 shrink-0 bg-ocean-dark flex flex-col border-r border-white/10">
        <div className="px-5 py-4 border-b border-white/10">
          <div className="flex items-center justify-between">
            <span className="font-display text-sm font-medium uppercase tracking-widest text-white">
              Motion Master
            </span>
            <span
              title={online ? 'API online' : 'API offline'}
              aria-label={online ? 'API online' : 'API offline'}
              className={`inline-block h-2.5 w-2.5 rounded-full ${
                online ? 'bg-status-good' : 'bg-status-bad animate-pulse'
              }`}
            />
          </div>
          <p className="text-white/30 text-xs font-display tracking-wider mt-0.5">v6.0.0-alpha.7</p>
        </div>

        <nav className="flex-1 overflow-y-auto py-4 [&::-webkit-scrollbar]:w-1 [&::-webkit-scrollbar-track]:bg-transparent [&::-webkit-scrollbar-thumb]:bg-white/20 [&::-webkit-scrollbar-thumb:hover]:bg-white/40">
          <NavItem to="/" label="Dashboard" />

          {online && (
            <>
              <NavItem to="/log" label="Log" />

              <p className="eyebrow px-5 mt-6 mb-1 text-white/40">Meta</p>
              <div className="pl-3">
                <NavItem to="/meta/esc-registers" label="ESC Registers" />
                <NavItem to="/meta/al-status-codes" label="AL Status Codes" />
                <NavItem to="/meta/foe-error-codes" label="FoE Error Codes" />
              </div>
            </>
          )}

          {online && hasScanned && (
            <p className="eyebrow px-5 mt-6 mb-1 text-white/40">
              {devices.length} {devices.length === 1 ? 'device' : 'devices'}
            </p>
          )}

          {online &&
            devices.map(d => (
              <DeviceSection key={d.slavePosition} deviceId={String(d.slavePosition)} />
            ))}
        </nav>

        <div className="px-5 py-3 border-t border-white/10">
          <p className="text-white/20 text-xs">© {new Date().getFullYear()} Synapticon GmbH</p>
        </div>
      </aside>

      {/* Content */}
      <main className="flex-1 overflow-auto bg-white">
        <Outlet />
      </main>

      <PwaUpdatePrompt />
    </div>
  )
}
