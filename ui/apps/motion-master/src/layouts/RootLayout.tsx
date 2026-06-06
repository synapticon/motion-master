import { useState } from 'react'
import { NavLink, Outlet } from 'react-router'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import PwaUpdatePrompt from '../components/PwaUpdatePrompt'
import { useConnection } from '../contexts/ConnectionContext'
import { useApiHealth } from '../hooks/useApiHealth'

const deviceLinks = [
  { to: 'foe', label: 'FoE' },
  { to: 'parameters', label: 'Parameters' },
  { to: 'registers', label: 'Registers' },
  { to: 'sii', label: 'SII' },
]

const AL_STATE_LABEL: Record<number, string> = {
  0: 'None', // No state — slave lost / powered off (SOEM reports state 0).
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

function alStateLabel(state?: DeviceState): string | null {
  if (!state) return null
  return AL_STATE_LABEL[state.alState] ?? `0x${state.alState.toString(16)}`
}

function NavItem({ to, label, end }: { to: string; label: string; end?: boolean }) {
  return (
    <NavLink
      to={to}
      end={end}
      className={({ isActive }) =>
        `block px-5 py-2 text-xs font-display uppercase tracking-widest border-l-2 transition-colors ${isActive
          ? 'border-syn-red text-white bg-white/10'
          : 'border-transparent text-white/60 hover:text-white hover:border-white/30'
        }`
      }
    >
      {label}
    </NavLink>
  )
}

function DeviceSection({
  deviceId,
  name,
  state,
}: {
  deviceId: string
  name?: string
  state?: DeviceState
}) {
  const { api } = useConnection()

  // Live presence probe. Read per-device (not batched) so one missing device reports
  // offline without disturbing the others. `null` while the first read is in flight.
  const onlineQuery = useQuery({
    queryKey: ['deviceOnline', deviceId],
    queryFn: () => api.getDeviceOnline(Number(deviceId)),
  })
  const online: boolean | null = onlineQuery.data?.data?.online ?? null
  const statusLabel = online === null ? 'Checking…' : online ? 'Online' : 'Offline'
  const statusTitle =
    online === null
      ? 'Checking… — probing whether the device responds on the bus'
      : online
        ? 'Online — the device answers live presence probes (mailbox reachable in PRE-OP or above)'
        : 'Offline — the device is not responding (in INIT, powered off, unplugged, or it left the bus)'
  const stateLabel = alStateLabel(state)

  return (
    <div className="mt-6">
      <div className="px-5 mb-2">
        {/* Identity */}
        <p className="eyebrow truncate">Device {deviceId}</p>
        {name && (
          <p className="text-white/30 text-[11px] tracking-wide truncate" title={name}>
            {name}
          </p>
        )}

        {/* Status — online presence on the left, AL state on the right */}
        <div className="mt-1.5 flex items-center justify-between gap-2">
          <span className="flex items-center gap-1.5 min-w-0">
            <span
              title={statusTitle}
              aria-label={statusLabel}
              className={`inline-block h-2 w-2 shrink-0 cursor-help rounded-full ring-2 ring-white ${online === null
                ? 'bg-white/30 animate-pulse'
                : online
                  ? 'bg-status-good shadow-[0_0_6px_1px_var(--color-status-good)]'
                  : 'bg-status-bad shadow-[0_0_6px_1px_var(--color-status-bad)]'
                }`}
            />
            <span
              title={statusTitle}
              className={`text-xs font-display tracking-wider cursor-help ${online ? 'text-white/70' : 'text-white/40'
                }`}
            >
              {statusLabel}
            </span>
          </span>
          {stateLabel && (
            <span
              title={`AL state${state?.error ? ' — error indicator set' : ''}`}
              className={`shrink-0 cursor-help px-1.5 py-0.5 rounded-sm text-[10px] font-display tracking-wider ${state?.error ? 'bg-status-warn/15 text-status-warn' : 'bg-white/10 text-white/60'
                }`}
            >
              {stateLabel}
              {state?.error && ' · err'}
            </span>
          )}
        </div>
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
  const { api, host, httpPort, hasScanned, isInitialized } = useConnection()
  const online = useApiHealth(api)
  const queryClient = useQueryClient()
  const [metaOpen, setMetaOpen] = useState(false)

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

  // `spinning` keeps the icon turning for at least one full rotation even when a
  // localhost refetch resolves in milliseconds — otherwise the spin is imperceptible.
  const [spinning, setSpinning] = useState(false)
  const refreshing = spinning || devicesQuery.isFetching || statesQuery.isFetching

  async function refreshAll() {
    setSpinning(true)
    const start = performance.now()
    try {
      const res = await devicesQuery.refetch()
      if ((res.data?.data.length ?? 0) > 0) {
        await statesQuery.refetch()
        await queryClient.invalidateQueries({ queryKey: ['deviceOnline'] })
      }
    } finally {
      const remaining = Math.max(0, 900 - (performance.now() - start))
      setTimeout(() => setSpinning(false), remaining)
    }
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
              title={
                online
                  ? `API online — the Motion Master HTTP server is reachable at https://${host}:${httpPort}`
                  : `API offline — no response from https://${host}:${httpPort}. Make sure Motion Master is installed and running on your system, then accept its TLS certificate.`
              }
              aria-label={online ? 'API online' : 'API offline'}
              className={`inline-block h-3 w-3 cursor-help rounded-full ring-2 ring-white ${online
                ? 'bg-status-good shadow-[0_0_8px_2px_var(--color-status-good)]'
                : 'bg-status-bad shadow-[0_0_8px_2px_var(--color-status-bad)] animate-pulse'
                }`}
            />
          </div>
          <p className="text-white/30 text-xs font-display tracking-wider mt-0.5">v6.0.0-alpha.18</p>
        </div>

        <nav className="flex-1 overflow-y-auto py-4 [&::-webkit-scrollbar]:w-1 [&::-webkit-scrollbar-track]:bg-transparent [&::-webkit-scrollbar-thumb]:bg-white/20 [&::-webkit-scrollbar-thumb:hover]:bg-white/40">
          <NavItem to="/" label="Connection" end />

          <p className="eyebrow text-white/40 px-5 mt-6 mb-1.5">Fieldbus</p>
          <NavItem to="/fieldbus" label="Control" />
          <NavItem to="/bus-config" label="Configuration" />
          <NavItem to="/process-image" label="Process Image" />
          <NavItem to="/bus-diagnostics" label="Diagnostics" />
          <NavItem to="/dc-sync" label="DC Sync" />

          <p className="eyebrow text-white/40 px-5 mt-6 mb-1.5">Data</p>
          <NavItem to="/monitorings" label="Monitorings" />

          <p className="eyebrow text-white/40 px-5 mt-6 mb-1.5">Server</p>
          <NavItem to="/log" label="Log" />
          <NavItem to="/requests" label="Requests" />

          <p className="eyebrow text-white/40 px-5 mt-6 mb-1.5">Reference</p>
          <NavItem to="/api-docs" label="API Docs" />

          {online && (
            <div className="mt-6">
              <button
                type="button"
                onClick={() => setMetaOpen(o => !o)}
                aria-expanded={metaOpen}
                className="group w-full flex items-center justify-between px-5 mb-1 cursor-pointer"
              >
                <span className="eyebrow text-white/40 group-hover:text-white/70 transition-colors">
                  Meta
                </span>
                <svg
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  strokeWidth="2"
                  strokeLinecap="round"
                  strokeLinejoin="round"
                  className={`h-3.5 w-3.5 text-white/40 group-hover:text-white/70 transition-transform ${metaOpen ? 'rotate-180' : ''
                    }`}
                >
                  <path d="m6 9 6 6 6-6" />
                </svg>
              </button>
              {metaOpen && (
                <div className="pl-3">
                  <NavItem to="/meta/al-status-codes" label="AL Status Codes" />
                  <NavItem to="/meta/data-types" label="Data Types" />
                  <NavItem to="/meta/esc-registers" label="ESC Registers" />
                  <NavItem to="/meta/foe-error-codes" label="FoE Error Codes" />
                </div>
              )}
            </div>
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
                    title="Refresh all devices — re-read the device list and AL states without re-scanning the bus (slaves keep their current state)"
                    aria-label="Refresh all devices — re-read the device list and AL states without re-scanning the bus"
                    className="text-white/40 hover:text-white disabled:opacity-40 disabled:cursor-default cursor-pointer transition-colors"
                  >
                    <svg
                      viewBox="0 0 24 24"
                      fill="none"
                      stroke="currentColor"
                      strokeWidth="2"
                      strokeLinecap="round"
                      strokeLinejoin="round"
                      className={`h-3.5 w-3.5 ${refreshing ? 'animate-spin' : ''}`}
                    >
                      <path d="M21 12a9 9 0 1 1-2.64-6.36" />
                      <path d="M21 3v6h-6" />
                    </svg>
                  </button>
                )}
              </div>
              <div
                className="flex items-center gap-2 cursor-help"
                title={
                  !isInitialized
                    ? 'Not initialized — no fieldbus driver is loaded. Initialize one on the Fieldbus page.'
                    : hasScanned
                      ? `Scanned — the bus was enumerated and ${devices.length} ${devices.length === 1 ? 'device is' : 'devices are'} present.`
                      : 'Initialized, not scanned — a driver is loaded but the bus has not been scanned yet, so no devices are known. Scan to discover them.'
                }
              >
                <span
                  className={`inline-block h-2 w-2 shrink-0 rounded-full ring-2 ring-white ${!isInitialized
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
                name={d.name}
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
