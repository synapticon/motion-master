import { useState } from 'react'
import { NavLink, Outlet } from 'react-router'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { BookOpenText, ChevronDown, Mail, RefreshCw } from 'lucide-react'
import PwaUpdatePrompt from '../components/PwaUpdatePrompt'
import SlavePositionBadge from '../components/SlavePositionBadge'
import { useConnection } from '../contexts/ConnectionContext'
import { usePreferences } from '../contexts/PreferencesContext'
import { useApiHealth } from '../hooks/useApiHealth'
import { formatHex } from '@synapticon/motion-master-client'

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

// Per-state badge colors (bg + text), matching the Transition-to-State buttons on
// the Control page so a state reads the same here as where it's commanded. BOOT is
// the odd one out (amber); the rest grade from neutral grey (no comms) up to green
// (fully operational). An error overrides these with red — see the badge below.
const AL_STATE_BADGE: Record<number, string> = {
  1: 'bg-grey-700 text-white',
  2: 'bg-ocean text-white',
  3: 'bg-status-warn text-grey-900',
  4: 'bg-status-info text-white',
  8: 'bg-green-600 text-white',
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
  vendorId,
  productCode,
  revisionNumber,
  serialNumber,
  state,
}: {
  deviceId: string
  name?: string
  vendorId?: number
  productCode?: number
  revisionNumber?: number
  serialNumber?: number
  state?: DeviceState
}) {
  const { api } = useConnection()

  // Links start expanded for every device. After that it's purely manual — the header
  // toggles it and that choice sticks; navigation alone never re-folds it.
  const [open, setOpen] = useState(true)

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

  // Object-dictionary read state. The backend enumerates a device's OD when it reaches
  // PRE-OP; we share the Parameters page's query cache (identical key) so this neither
  // double-fetches nor drifts out of sync with that page. `null` while the first read is
  // in flight; an empty list means the OD has not been read yet.
  const parametersQuery = useQuery({
    queryKey: ['deviceParameters', Number(deviceId)],
    queryFn: () => api.getDeviceParameters(Number(deviceId)),
    staleTime: Infinity,
    retry: false,
  })
  const parameterCount: number | null = parametersQuery.isPending
    ? null
    : parametersQuery.data?.data?.length ?? 0
  const dictionaryLabel =
    parameterCount === null
      ? 'Reading object dictionary'
      : parameterCount > 0
        ? 'Object dictionary read'
        : 'Object dictionary not read'
  const dictionaryTitle =
    parameterCount === null
      ? 'Object dictionary — reading the device’s parameters…'
      : parameterCount > 0
        ? `Object dictionary — ${parameterCount} parameters read (the device’s CoE object dictionary has been enumerated; it is read automatically once the device reaches PRE-OP)`
        : 'Object dictionary — not read yet: no parameters enumerated. The device reads its OD on reaching PRE-OP; open the Parameters page to read it on demand.'

  const stateLabel = alStateLabel(state)

  // Full device identity, shown as a multi-line tooltip on the name — the immutable
  // EEPROM identity that the status line deliberately keeps off the chrome.
  const identityTitle = [
    'Name',
    name ?? `Device ${deviceId}`,
    ...(vendorId !== undefined ? ['Vendor ID', formatHex(vendorId, 8)] : []),
    ...(productCode !== undefined ? ['Product Code', formatHex(productCode, 8)] : []),
    ...(revisionNumber !== undefined ? ['Revision', formatHex(revisionNumber, 8)] : []),
    ...(serialNumber !== undefined ? ['Serial', String(serialNumber)] : []),
  ].join('\n')

  return (
    <div className="mt-2">
      {/* Device card — the header toggles the links below; the AL-state badge in the
          status line carries the state color, so the card needs no left accent bar. */}
      <button
        type="button"
        onClick={() => setOpen(o => !o)}
        aria-expanded={open}
        className="group w-full text-left px-5 py-2.5 bg-white/[0.06] hover:bg-white/[0.1] border-y border-white/10 transition-colors cursor-pointer"
      >
        {/* Identity — slave position leads on the left, then the device name. */}
        <div className="flex items-center justify-between gap-2">
          <div className="flex items-center gap-2 min-w-0">
            <SlavePositionBadge position={Number(deviceId)} tone="muted" />
            <span
              className="truncate font-display text-sm tracking-wide text-white/90 cursor-help"
              title={identityTitle}
            >
              {name ?? `Device ${deviceId}`}
            </span>
          </div>
          <ChevronDown
            className={`h-3.5 w-3.5 shrink-0 text-white/40 group-hover:text-white/70 transition-transform ${open ? 'rotate-180' : ''
              }`}
          />
        </div>

        {/* Status — one quiet line under the identity row: AL state badge, then two glyphs
            (mailbox presence, object-dictionary read state), each green when healthy and
            grey otherwise. Immutable identity (vendor/code/revision/serial) lives in the
            name tooltip. */}
        <div className="mt-1.5 flex items-center gap-1 text-[10px] font-display tracking-wider">
          {stateLabel && (
            <span
              className={`shrink-0 inline-flex items-center h-[18px] px-1.5 rounded-sm ${state?.error
                ? 'bg-status-bad text-white'
                : AL_STATE_BADGE[state?.alState ?? -1] ?? 'bg-white/10 text-white/60'
                }`}
              title={
                state?.error
                  ? 'AL state — the device’s EtherCAT Application Layer state, the low nibble of the AL Status register (0x0130). The error indicator (bit 4) is set; the reason is in the AL Status Code register (0x0134).'
                  : 'AL state — the device’s EtherCAT Application Layer state, the low nibble of the AL Status register (0x0130).'
              }
            >
              {stateLabel}
              {state && ` (${state.alState})`}
              {state?.error && ' · err'}
            </span>
          )}
          <span
            title={statusTitle}
            aria-label={statusLabel}
            className={`shrink-0 flex items-center justify-center h-[18px] px-1.5 rounded-sm ${mailboxActive === null
              ? 'bg-white/15 animate-pulse'
              : mailboxActive
                ? 'bg-green-600'
                : 'bg-white/15'
              }`}
          >
            <Mail className="h-3 w-3 text-white" />
          </span>
          <span
            title={dictionaryTitle}
            aria-label={dictionaryLabel}
            className={`shrink-0 flex items-center justify-center h-[18px] px-1.5 rounded-sm ${parameterCount === null
              ? 'bg-white/15 animate-pulse'
              : parameterCount > 0
                ? 'bg-green-600'
                : 'bg-white/15'
              }`}
          >
            <BookOpenText className="h-3 w-3 text-white" />
          </span>
        </div>
      </button>
      {open && (
        <div>
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
      <aside className="w-60 shrink-0 bg-ocean-dark flex flex-col border-r border-white/10">
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
          <p className="text-white/30 text-xs font-display tracking-wider mt-0.5">v6.0.0-alpha.23</p>
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
              <ChevronDown
                className={`h-3.5 w-3.5 text-white/40 group-hover:text-white/70 transition-transform ${prefsOpen ? 'rotate-180' : ''
                  }`}
              />
            </button>
            {prefsOpen && (
              <label className="flex items-center gap-2 px-5 py-2 text-xs text-white/60 hover:text-white cursor-pointer select-none">
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
          <NavItem to="/recorder" label="Recorder" />
          <NavItem to="/parameter-caches" label="Parameter Caches" />

          <p className="eyebrow text-white/40 px-5 mt-6 mb-1.5">Server</p>
          <NavItem to="/log" label="Log" />
          <NavItem to="/requests" label="Requests" />

          <p className="eyebrow text-white/40 px-5 mt-6 mb-1.5">Tools</p>
          <NavItem to="/tools/sii" label="SII" />

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
                <ChevronDown
                  className={`h-3.5 w-3.5 text-white/40 group-hover:text-white/70 transition-transform ${metaOpen ? 'rotate-180' : ''
                    }`}
                />
              </button>
              {metaOpen && (
                <div>
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
                <p
                  className={`eyebrow text-white/40 ${hasScanned ? 'cursor-help' : ''}`}
                  title={
                    hasScanned
                      ? `Scanned — the bus was enumerated and ${devices.length} ${devices.length === 1 ? 'device is' : 'devices are'} present.`
                      : undefined
                  }
                >
                  Devices{hasScanned && ` (${devices.length})`}
                </p>
                {hasScanned && (
                  <button
                    type="button"
                    onClick={refreshAll}
                    disabled={refreshing}
                    title="Refresh all devices — re-read the device list and AL states without re-scanning the bus (slaves keep their current state)"
                    aria-label="Refresh all devices — re-read the device list and AL states without re-scanning the bus"
                    className="text-white/40 hover:text-white disabled:opacity-40 disabled:cursor-default cursor-pointer transition-colors"
                  >
                    <RefreshCw className={`h-3.5 w-3.5 ${refreshing ? 'animate-spin' : ''}`} />
                  </button>
                )}
              </div>
              {!hasScanned && (
                <div
                  className="cursor-help"
                  title={
                    !isInitialized
                      ? 'Not initialized — no fieldbus driver is loaded. Initialize one on the Fieldbus page.'
                      : 'Initialized, not scanned — a driver is loaded but the bus has not been scanned yet, so no devices are known. Scan to discover them.'
                  }
                >
                  <span
                    className={`text-xs font-display tracking-wider ${!isInitialized ? 'text-white/40' : 'text-status-warn'
                      }`}
                  >
                    {!isInitialized ? 'Not initialized' : 'Initialized · not scanned'}
                  </span>
                </div>
              )}
            </div>
          )}

          {online &&
            devices.map(d => (
              <DeviceSection
                key={d.slavePosition}
                deviceId={String(d.slavePosition)}
                name={d.name}
                vendorId={d.vendorId}
                productCode={d.productCode}
                revisionNumber={d.revisionNumber}
                serialNumber={d.serialNumber}
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
