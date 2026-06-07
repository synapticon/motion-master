import { useState } from 'react'
import { NavLink, Outlet, useLocation } from 'react-router'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import PwaUpdatePrompt from '../components/PwaUpdatePrompt'
import { useConnection } from '../contexts/ConnectionContext'
import { usePreferences } from '../contexts/PreferencesContext'
import { useApiHealth } from '../hooks/useApiHealth'
import { formatHex } from '../utils/hex'

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
  productCode,
  state,
}: {
  deviceId: string
  name?: string
  productCode?: number
  state?: DeviceState
}) {
  const { api } = useConnection()

  // Initial fold state is route-driven: on load (or reload) the device whose page is open
  // starts expanded, the rest collapsed. After that it's purely manual — the header toggles
  // it and that choice sticks; navigation alone never re-expands it.
  const location = useLocation()
  const isActive = location.pathname.startsWith(`/devices/${deviceId}/`)
  const [open, setOpen] = useState(isActive)

  // Live mailbox probe. Read per-device (not batched) so one missing device reports
  // inactive without disturbing the others. `null` while the first read is in flight.
  const mailboxQuery = useQuery({
    queryKey: ['deviceMailboxActive', deviceId],
    queryFn: () => api.getDeviceMailboxActive(Number(deviceId)),
  })
  const mailboxActive: boolean | null = mailboxQuery.data?.data?.mailboxActive ?? null
  const statusLabel =
    mailboxActive === null ? 'Checking…' : mailboxActive ? 'Mailbox active' : 'Mailbox inactive'
  const statusTitle =
    mailboxActive === null
      ? 'Checking… — reading the device’s AL state on the bus'
      : mailboxActive
        ? 'Mailbox active — the device is in PRE-OP or higher, so its CoE/SDO mailbox answers (regardless of the AL error flag, which the state badge shows separately)'
        : 'Mailbox inactive — no CoE/SDO mailbox: the device is in INIT or BOOT, or is not responding (powered off, unplugged, or left the bus)'
  const stateLabel = alStateLabel(state)

  return (
    <div className="mt-2">
      {/* Backgrounded identity header that doubles as the fold toggle for the links below. */}
      <button
        type="button"
        onClick={() => setOpen(o => !o)}
        aria-expanded={open}
        className="group w-full text-left px-5 py-2 bg-white/[0.06] hover:bg-white/[0.1] border-y border-white/10 transition-colors cursor-pointer"
      >
        {/* Identity */}
        <div className="flex items-start justify-between gap-2">
          <div className="flex items-center gap-2 min-w-0 text-[13px]">
            <span
              className="shrink-0 cursor-help rounded-sm bg-syn-red px-1.5 py-0.5 font-mono font-semibold text-white"
              title={`Slave position — the device’s 1-based position on the EtherCAT bus. This is the {slavePosition} used in API endpoint paths, e.g. /api/devices/${deviceId}/parameters`}
            >
              {deviceId.padStart(2, '0')}
            </span>
            {name && (
              <span className="shrink-0 tracking-wide text-white/80" title={name}>
                {name.length > 8 ? `${name.slice(0, 8).trimEnd()}…` : name}
              </span>
            )}
            {productCode !== undefined && (
              <span
                className="truncate font-mono text-white/40"
                title={`Product code (EEPROM): ${formatHex(productCode)}`}
              >
                {formatHex(productCode)}
              </span>
            )}
          </div>
          <svg
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            strokeWidth="2"
            strokeLinecap="round"
            strokeLinejoin="round"
            className={`mt-0.5 h-3.5 w-3.5 shrink-0 text-white/40 group-hover:text-white/70 transition-transform ${open ? 'rotate-180' : ''
              }`}
          >
            <path d="m6 9 6 6 6-6" />
          </svg>
        </div>

        {/* Status — AL state first (primary), online presence as a small derived icon */}
        <div className="mt-1.5 flex items-center gap-2">
          {stateLabel && (
            <span
              title={
                state?.error
                  ? 'AL state — the device’s EtherCAT Application Layer state, the low nibble of the AL Status register (0x0130). The error indicator (bit 4) is set; the reason is in the AL Status Code register (0x0134).'
                  : 'AL state — the device’s EtherCAT Application Layer state, the low nibble of the AL Status register (0x0130).'
              }
              className={`shrink-0 px-1.5 py-0.5 rounded-sm text-[10px] font-display tracking-wider ${state?.error ? 'bg-status-warn/15 text-status-warn' : 'bg-white/10 text-white/60'
                }`}
            >
              {stateLabel}
              {state?.error && ' · err'}
            </span>
          )}
          <span
            title={statusTitle}
            aria-label={statusLabel}
            className="shrink-0 flex items-center gap-1.5"
          >
            <span
              className={`inline-block h-2 w-2 rounded-[1px] ${mailboxActive === null
                ? 'bg-white/40 animate-pulse'
                : mailboxActive
                  ? 'bg-status-good'
                  : 'bg-status-bad'
                }`}
            />
            <span className="text-[10px] font-display tracking-wider text-white/70">
              {statusLabel}
            </span>
          </span>
        </div>
      </button>
      {open && (
        <div className="pl-3">
          {deviceLinks.map(({ to, label }) => (
            <NavItem key={to} to={`/devices/${deviceId}/${to}`} label={label} />
          ))}
        </div>
      )}
    </div>
  )
}

export default function RootLayout() {
  const { api, host, httpPort, hasScanned, isInitialized } = useConnection()
  const online = useApiHealth(api)
  const queryClient = useQueryClient()
  const [metaOpen, setMetaOpen] = useState(false)
  const [prefsOpen, setPrefsOpen] = useState(false)
  const { hintsInline, setHintsInline } = usePreferences()

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
        await queryClient.invalidateQueries({ queryKey: ['deviceMailboxActive'] })
      }
    } finally {
      const remaining = Math.max(0, 900 - (performance.now() - start))
      setTimeout(() => setSpinning(false), remaining)
    }
  }

  return (
    <div className="flex h-screen bg-grey-50 text-grey-900">
      {/* Sidebar — Ocean Dark */}
      <aside className="w-72 shrink-0 bg-ocean-dark flex flex-col border-r border-white/10">
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
          <p className="text-white/30 text-xs font-display tracking-wider mt-0.5">v6.0.0-alpha.19</p>
        </div>

        <nav className="flex-1 overflow-y-auto py-4 [&::-webkit-scrollbar]:w-1 [&::-webkit-scrollbar-track]:bg-transparent [&::-webkit-scrollbar-thumb]:bg-white/20 [&::-webkit-scrollbar-thumb:hover]:bg-white/40">
          <NavItem to="/" label="Connection" end />

          <div className="mt-6">
            <button
              type="button"
              onClick={() => setPrefsOpen(o => !o)}
              aria-expanded={prefsOpen}
              className="group w-full flex items-center justify-between px-5 mb-1 cursor-pointer"
            >
              <span className="eyebrow text-white/40 group-hover:text-white/70 transition-colors">
                Preferences
              </span>
              <svg
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                strokeWidth="2"
                strokeLinecap="round"
                strokeLinejoin="round"
                className={`h-3.5 w-3.5 text-white/40 group-hover:text-white/70 transition-transform ${prefsOpen ? 'rotate-180' : ''
                  }`}
              >
                <path d="m6 9 6 6 6-6" />
              </svg>
            </button>
            {prefsOpen && (
              <label className="flex items-center gap-2 px-5 py-2 text-[11px] text-white/50 hover:text-white/80 cursor-pointer select-none">
                <input
                  type="checkbox"
                  checked={hintsInline}
                  onChange={e => setHintsInline(e.target.checked)}
                  className="accent-syn-red"
                />
                Show hints inline
              </label>
            )}
          </div>

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
                className="cursor-help"
                title={
                  !isInitialized
                    ? 'Not initialized — no fieldbus driver is loaded. Initialize one on the Fieldbus page.'
                    : hasScanned
                      ? `Scanned — the bus was enumerated and ${devices.length} ${devices.length === 1 ? 'device is' : 'devices are'} present.`
                      : 'Initialized, not scanned — a driver is loaded but the bus has not been scanned yet, so no devices are known. Scan to discover them.'
                }
              >
                <span
                  className={`text-xs font-display tracking-wider ${!isInitialized
                    ? 'text-white/40'
                    : hasScanned
                      ? 'text-status-good'
                      : 'text-status-warn'
                    }`}
                >
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
                productCode={d.productCode}
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
