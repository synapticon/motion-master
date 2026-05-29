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

const AL_STATE_LABEL: Record<number, string> = {
  1: 'Init',
  2: 'PreOp',
  3: 'Boot',
  4: 'SafeOp',
  8: 'Op',
}

interface DeviceState {
  alState: number
  error: boolean
}

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

function DeviceSection({ deviceId, state }: { deviceId: string; state?: DeviceState }) {
  return (
    <div className="mt-6">
      <div className="px-5 mb-2 flex items-baseline justify-between gap-2">
        <p className="eyebrow">Device {deviceId}</p>
        {state && (
          <span
            className={`text-xs font-display tracking-wider ${
              state.error ? 'text-status-warn' : 'text-white/50'
            }`}
          >
            {AL_STATE_LABEL[state.alState] ?? `0x${state.alState.toString(16)}`}
            {state.error && ' · error'}
          </span>
        )}
      </div>
      <div className="pl-3">
        {deviceLinks.map(({ to, label }) => (
          <NavItem key={to} to={`/devices/${deviceId}/${to}`} label={label} />
        ))}
      </div>
    </div>
  )
}

export default function RootLayout() {
  const { api, hasScanned, isInitialized } = useConnection()
  const online = useApiHealth(api)

  const devicesQuery = useQuery({
    queryKey: ['devices'],
    queryFn: () => api.getDevices(),
    enabled: hasScanned,
  })

  const devices = devicesQuery.data?.data ?? []

  const statesQuery = useQuery({
    queryKey: ['deviceStates'],
    queryFn: () => api.getDeviceStates(),
    enabled: hasScanned && devices.length > 0,
  })

  const stateByPosition = new Map<number, DeviceState>(
    (statesQuery.data?.data ?? []).map(s => [s.slavePosition, s]),
  )

  const refreshing = devicesQuery.isFetching || statesQuery.isFetching

  async function refreshAll() {
    const res = await devicesQuery.refetch()
    if ((res.data?.data.length ?? 0) > 0) await statesQuery.refetch()
  }

  return (
    <div className="flex h-screen bg-grey-50 text-grey-900">
      {/* Sidebar — Ocean Dark */}
      <aside className="w-64 shrink-0 bg-ocean-dark flex flex-col border-r border-white/10">
        <div className="px-5 py-4 border-b border-white/10">
          <div className="flex items-center justify-between">
            <span className="font-display text-sm font-medium uppercase tracking-widest text-white">
              Motion Master
            </span>
            <span
              title={online ? 'API online' : 'API offline — Start motion-master'}
              aria-label={online ? 'API online' : 'API offline — Start motion-master'}
              className={`inline-block h-3 w-3 rounded-full ring-2 ring-white ${
                online
                  ? 'bg-status-good shadow-[0_0_8px_2px_var(--color-status-good)]'
                  : 'bg-status-bad shadow-[0_0_8px_2px_var(--color-status-bad)] animate-pulse'
              }`}
            />
          </div>
          <p className="text-white/30 text-xs font-display tracking-wider mt-0.5">v6.0.0-alpha.12</p>
        </div>

        <nav className="flex-1 overflow-y-auto py-4 [&::-webkit-scrollbar]:w-1 [&::-webkit-scrollbar-track]:bg-transparent [&::-webkit-scrollbar-thumb]:bg-white/20 [&::-webkit-scrollbar-thumb:hover]:bg-white/40">
          <NavItem to="/" label="Dashboard" />
          <NavItem to="/log" label="Log" />
          <NavItem to="/requests" label="Requests" />

          {online && (
            <>
              <p className="eyebrow px-5 mt-6 mb-1 text-white/40">Meta</p>
              <div className="pl-3">
                <NavItem to="/meta/esc-registers" label="ESC Registers" />
                <NavItem to="/meta/al-status-codes" label="AL Status Codes" />
                <NavItem to="/meta/foe-error-codes" label="FoE Error Codes" />
                <NavItem to="/meta/data-types" label="Data Types" />
              </div>
            </>
          )}

          {online && (
            <div className="px-5 mt-6 mb-2">
              <div className="flex items-center justify-between mb-1.5">
                <p className="eyebrow text-white/40">Fieldbus</p>
                {hasScanned && (
                  <button
                    type="button"
                    onClick={refreshAll}
                    disabled={refreshing}
                    title="Refresh all devices"
                    aria-label="Refresh all devices"
                    className="text-white/40 hover:text-white disabled:opacity-40 disabled:cursor-default cursor-pointer transition-colors"
                  >
                    <svg
                      viewBox="0 0 24 24"
                      fill="none"
                      stroke="currentColor"
                      strokeWidth="2"
                      strokeLinecap="round"
                      strokeLinejoin="round"
                      className="h-3.5 w-3.5"
                    >
                      <path d="M21 12a9 9 0 1 1-2.64-6.36" />
                      <path d="M21 3v6h-6" />
                    </svg>
                  </button>
                )}
              </div>
              <div className="flex items-center gap-2">
                <span
                  className={`inline-block h-2 w-2 shrink-0 rounded-full ring-2 ring-white ${
                    !isInitialized
                      ? 'bg-white/30'
                      : hasScanned
                        ? 'bg-status-good shadow-[0_0_6px_1px_var(--color-status-good)]'
                        : 'bg-status-warn shadow-[0_0_6px_1px_var(--color-status-warn)]'
                  }`}
                />
                <span className="text-xs font-display tracking-wider text-white/70">
                  {!isInitialized
                    ? 'Not initialized'
                    : hasScanned
                      ? `${devices.length} ${devices.length === 1 ? 'device' : 'devices'}`
                      : 'Initialized · not scanned'}
                </span>
              </div>
            </div>
          )}

          {online &&
            devices.map(d => (
              <DeviceSection
                key={d.slavePosition}
                deviceId={String(d.slavePosition)}
                state={stateByPosition.get(d.slavePosition)}
              />
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
