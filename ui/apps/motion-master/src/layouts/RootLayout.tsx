import { NavLink, Outlet, useParams } from 'react-router'

const deviceLinks = [
  { to: 'ethercat-state', label: 'EtherCAT State' },
  { to: 'object-dictionary', label: 'Object Dictionary' },
  { to: 'sii', label: 'SII' },
  { to: 'registers', label: 'Registers' },
  { to: 'foe', label: 'FoE' },
  { to: 'process-data', label: 'Process Data' },
]

function DeviceNav({ deviceId }: { deviceId: string }) {
  return (
    <div className="mt-4">
      <p className="px-3 mb-1 text-xs font-semibold text-gray-500 uppercase tracking-wider">
        Device {deviceId}
      </p>
      {deviceLinks.map(({ to, label }) => (
        <NavLink
          key={to}
          to={`/devices/${deviceId}/${to}`}
          className={({ isActive }) =>
            `block px-3 py-1.5 rounded text-sm ${
              isActive
                ? 'bg-gray-700 text-white'
                : 'text-gray-400 hover:text-white hover:bg-gray-800'
            }`
          }
        >
          {label}
        </NavLink>
      ))}
    </div>
  )
}

export default function RootLayout() {
  const { deviceId } = useParams()

  return (
    <div className="flex h-screen bg-gray-950 text-gray-100">
      <aside className="w-56 shrink-0 border-r border-gray-800 flex flex-col">
        <div className="p-4 border-b border-gray-800">
          <span className="text-sm font-semibold tracking-wide">Motion Master</span>
        </div>
        <nav className="flex-1 overflow-y-auto p-2">
          <NavLink
            to="/"
            end
            className={({ isActive }) =>
              `block px-3 py-1.5 rounded text-sm ${
                isActive
                  ? 'bg-gray-700 text-white'
                  : 'text-gray-400 hover:text-white hover:bg-gray-800'
              }`
            }
          >
            Dashboard
          </NavLink>
          {deviceId && <DeviceNav deviceId={deviceId} />}
        </nav>
      </aside>
      <main className="flex-1 overflow-auto p-6">
        <Outlet />
      </main>
    </div>
  )
}
