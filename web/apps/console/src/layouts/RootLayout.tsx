import { useState, type ReactNode } from 'react'
import { NavLink, Outlet } from 'react-router'
import { useQuery } from '@tanstack/react-query'
import { BookOpenText, ChevronDown, Mail, RefreshCw } from 'lucide-react'
import PwaUpdatePrompt from '../components/PwaUpdatePrompt'
import SlavePositionBadge from '../components/SlavePositionBadge'
import { useConnection } from '../contexts/ConnectionContext'
import { formatHex } from '@synapticon/motion-master-client'

const deviceLinks = [
  { to: 'foe', label: 'FoE' },
  { to: 'parameters', label: 'Parameters' },
  { to: 'pdo-mapping', label: 'PDO Mapping' },
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

// Mailbox (CoE/SDO) communication is live in PRE-OP, SAFE-OP and OP — independent of the
// AL error flag (a device in SAFE-OP+error still answers). INIT has no mailbox and BOOT's
// is FoE-only, so both count as inactive. Mirrors Device::mailboxActive() on the backend, so
// it can be derived from the batched device-state poll instead of a per-device request.
const MAILBOX_ACTIVE_STATES = new Set([2, 4, 8])

function mailboxActiveFor(state?: DeviceState): boolean | null {
  if (!state) return null
  return MAILBOX_ACTIVE_STATES.has(state.alState)
}

function NavItem({
  to,
  label,
  end,
  trailing,
}: {
  to: string
  label: string
  end?: boolean
  trailing?: ReactNode
}) {
  return (
    <NavLink
      to={to}
      end={end}
      className={({ isActive }) =>
        `flex items-center justify-between gap-2 px-5 py-2 text-xs font-display uppercase tracking-wide border-l-2 transition-colors ${isActive
          ? 'border-syn-red text-white bg-white/10'
          : 'border-transparent text-white/60 hover:text-white hover:border-white/30'
        }`
      }
    >
      <span>{label}</span>
      {trailing}
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

  // Mailbox availability is a pure function of the AL state, so we derive it from the
  // batched device-state poll rather than issuing a per-device request (which would be one
  // request per device every few seconds — N requests for N devices). `null` while no state
  // has arrived yet.
  const mailboxActive: boolean | null = mailboxActiveFor(state)
  // BOOT replaces the standard CoE/SDO mailbox with a FoE-only one (firmware transfer is the
  // only thing the device can do there), so we swap the mail glyph for a "FoE" tag and a tooltip
  // that says so, rather than showing the mailbox as plainly inactive.
  const isBoot = state?.alState === 3
  const statusLabel =
    mailboxActive === null
      ? 'Checking…'
      : isBoot
        ? 'FoE only'
        : mailboxActive
          ? 'Mailbox active'
          : 'Mailbox inactive'
  const statusTitle =
    mailboxActive === null
      ? 'Checking… — reading the device’s AL state on the bus'
      : isBoot
        ? 'FoE only — the device is in BOOT, so its mailbox carries only File-over-EtherCAT (firmware transfer); CoE/SDO is unavailable until it leaves BOOT'
        : mailboxActive
          ? 'Mailbox active — the device is in PRE-OP or higher, so its CoE/SDO mailbox answers (regardless of the AL error flag, which the state badge shows separately)'
          : 'Mailbox inactive — no CoE/SDO mailbox: the device is in INIT, or is not responding (powered off, unplugged, or left the bus)'

  // Object-dictionary read state. The backend enumerates a device's OD when it reaches
  // PRE-OP; we share the Parameters page's query cache (identical key) so this neither
  // double-fetches nor drifts out of sync with that page. `null` while the first read is
  // in flight; an empty list means the OD has not been read yet.
  const parametersQuery = useQuery({
    queryKey: ['deviceParameters', Number(deviceId)],
    queryFn: () => api.getDeviceParameters(Number(deviceId)),
    staleTime: Infinity,
    retry: false,
    // The backend reads the OD automatically once the device reaches PRE-OP, which is often
    // after this query first runs and returns an empty list. With staleTime: Infinity that empty
    // result would stick forever. Poll while empty so the green dictionary icon (and the shared
    // Parameters page cache) pick up the auto-read; stop polling the moment it is populated.
    refetchInterval: (query) => ((query.state.data?.data?.length ?? 0) > 0 ? false : 2000),
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
        <div className="mt-1.5 flex items-center gap-1 text-[10px] font-display tracking-wide">
          {stateLabel && (
            <span
              className={`shrink-0 inline-flex items-center h-[18px] px-1.5 rounded-sm cursor-help ${state?.error
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
            className={`shrink-0 flex items-center justify-center h-[18px] px-1.5 rounded-sm cursor-help ${mailboxActive === null
              ? 'bg-white/15 animate-pulse'
              : isBoot
                ? 'bg-status-warn text-grey-900'
                : mailboxActive
                  ? 'bg-green-600'
                  : 'bg-white/15'
              }`}
          >
            {isBoot ? (
              <span className="font-display font-semibold leading-none">FoE</span>
            ) : (
              <Mail className="h-3 w-3 text-white" />
            )}
          </span>
          <span
            title={dictionaryTitle}
            aria-label={dictionaryLabel}
            className={`shrink-0 flex items-center justify-center h-[18px] px-1.5 rounded-sm cursor-help ${parameterCount === null
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
  const { api, host, httpPort, online, hasScanned, isInitialized } = useConnection()
  const [metaOpen, setMetaOpen] = useState(false)

  // `hasScanned` is sticky (cleared only on reset), so it stays true after the server
  // drops. Gate on `online` too, or these keep firing against a dead API — the states
  // poll especially, every 3s — cluttering the Requests log for nothing.
  const devicesQuery = useQuery({
    queryKey: ['devices'],
    queryFn: () => api.getDevices(),
    enabled: online && hasScanned,
  })

  const devices = devicesQuery.data?.data ?? []

  const statesQuery = useQuery({
    queryKey: ['deviceStates'],
    queryFn: () => api.getDeviceStates(),
    enabled: online && hasScanned && devices.length > 0,
    // AL state changes out-of-band (other clients, the drive's own faults, manual
    // transitions), so poll a few-second cadence to keep the sidebar badges live.
    refetchInterval: 3000,
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
        <div className="flex flex-col items-center border-b border-white/10 bg-black/20 px-5 py-4">
          <img
            src={`${import.meta.env.BASE_URL}pwa-192x192.png`}
            alt="Motion Master logo"
            width={56}
            height={56}
            title={online ? 'API online' : 'API offline'}
            className={`h-14 w-14 rounded-md transition-shadow ${online
              ? 'ring-2 ring-status-good/30 shadow-[0_0_16px_3px_var(--color-status-good)]'
              : 'ring-1 ring-white/10'
              }`}
          />
          <span className="mt-3 font-display text-sm font-medium uppercase tracking-widest text-white">
            Motion Master
          </span>
          <p className="mt-0.5 text-xs font-display tracking-wide text-white/30">v6.0.0-alpha.41</p>
        </div>

        <nav className="flex-1 overflow-y-auto py-4 [&::-webkit-scrollbar]:w-1 [&::-webkit-scrollbar-track]:bg-transparent [&::-webkit-scrollbar-thumb]:bg-white/20 [&::-webkit-scrollbar-thumb:hover]:bg-white/40">
          <NavItem
            to="/"
            label="Connection"
            end
            trailing={
              <span
                title={
                  online
                    ? `API online — the Motion Master HTTP server is reachable at https://${host}:${httpPort}`
                    : `API offline — no response from https://${host}:${httpPort}. Make sure Motion Master is installed and running on your system.`
                }
                aria-label={online ? 'API online' : 'API offline'}
                className={`inline-block h-2.5 w-2.5 shrink-0 cursor-help rounded-full ring-2 ring-white ${online
                  ? 'bg-status-good shadow-[0_0_8px_2px_var(--color-status-good)]'
                  : 'bg-status-bad shadow-[0_0_6px_2px_var(--color-status-bad)] animate-pulse'
                  }`}
              />
            }
          />

          {/* Sidebar links unlock in tiers by how much of the stack is live:
              - API online: the group headings + the pages that work without slaves —
                Control (where init/scan/reset happen) and Parameter Caches (reads the
                on-disk OD cache, independent of any live scan).
              - Bus scanned (hasScanned): the per-slave bus views and the process-data /
                monitoring pages, which need discovered devices to show anything. */}
          {online && (
            <>
              <p className="eyebrow text-white/40 px-5 mt-6 mb-1.5">Fieldbus</p>
              <NavItem to="/fieldbus/control" label="Control" />
              {hasScanned && (
                <>
                  <NavItem to="/fieldbus/configuration" label="Configuration" />
                  <NavItem to="/fieldbus/process-image" label="Process Image" />
                  <NavItem to="/fieldbus/diagnostics" label="Diagnostics" />
                  <NavItem to="/fieldbus/dc-sync" label="DC Sync" />
                </>
              )}

              <p className="eyebrow text-white/40 px-5 mt-6 mb-1.5">Data</p>
              {hasScanned && (
                <>
                  <NavItem to="/data/process-data" label="Process Data" />
                  <NavItem to="/data/monitorings" label="Monitorings" />
                  <NavItem to="/data/recorder" label="Recorder" />
                </>
              )}
              <NavItem to="/data/parameter-caches" label="Parameter Caches" />
            </>
          )}

          <p className="eyebrow text-white/40 px-5 mt-6 mb-1.5">Server</p>
          {/* Game Loop and Log read from the API (the loop runs unconditionally, so
              Game Loop needs no scan); Requests is a purely client-side log of HTTP
              requests (failures included), so it stays visible — it's most useful
              precisely when the connection is failing. */}
          {online && <NavItem to="/server/game-loop" label="Game Loop" />}
          {online && <NavItem to="/server/log" label="Log" />}
          <NavItem to="/server/requests" label="Requests" />

          {online && (
            <>
              <p className="eyebrow text-white/40 px-5 mt-6 mb-1.5">Tools</p>
              <NavItem to="/tools/sii" label="SII" />
            </>
          )}

          <p className="eyebrow text-white/40 px-5 mt-6 mb-1.5">Reference</p>
          <NavItem to="/reference/api-docs" label="API Docs" />

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
                  <NavItem to="/meta/esc-registers" label="ESC Registers" />
                  <NavItem to="/meta/foe-error-codes" label="FoE Error Codes" />
                  <NavItem to="/meta/object-data-types" label="Object Data Types" />
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
                      ? 'Not initialized — no fieldbus driver is loaded. Initialize one on the Control page.'
                      : 'Initialized, not scanned — a driver is loaded but the bus has not been scanned yet, so no devices are known. Scan to discover them.'
                  }
                >
                  <span
                    className={`text-xs font-display tracking-wide ${!isInitialized ? 'text-white/40' : 'text-status-warn'
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

        <div className="px-5 py-3 border-t border-white/10 bg-black/20">
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
