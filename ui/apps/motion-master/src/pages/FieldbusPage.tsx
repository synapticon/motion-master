import { useEffect, useRef, useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'

const AL_STATES = [
  { value: 1 as const, label: 'Init' },
  { value: 2 as const, label: 'PreOp' },
  { value: 3 as const, label: 'Boot' },
  { value: 4 as const, label: 'SafeOp' },
  { value: 8 as const, label: 'Op' },
]

function apiError(err: unknown): string {
  if (err && typeof err === 'object' && 'error' in err) {
    const inner = (err as { error: unknown }).error
    if (inner && typeof inner === 'object' && 'error' in inner) {
      return String((inner as { error: unknown }).error)
    }
  }
  return 'Unknown error'
}

function apiStatus(err: unknown): number | undefined {
  if (err && typeof err === 'object' && 'status' in err) {
    return (err as { status?: number }).status
  }
  return undefined
}

const inputCls = 'border border-grey-300 px-3 py-2 text-sm w-full bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'
const btnCls =
  'bg-syn-red text-white px-4 py-2 text-xs hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer w-full transition-colors'
const btnOutlineCls =
  'border border-syn-red text-syn-red px-3 py-1.5 text-xs hover:bg-syn-red hover:text-white disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

export default function FieldbusPage() {
  const queryClient = useQueryClient()
  const { api, driver, setDriver, adapter, setAdapter, hasScanned, setHasScanned, setIsInitialized, alreadyInitialized, setAlreadyInitialized } = useConnection()
  const [alState, setAlState] = useState<1 | 2 | 3 | 4 | 8>(2)
  const AL_STATE_LABEL: Record<number, string> = { 1: 'Init', 2: 'PreOp', 3: 'Boot', 4: 'SafeOp', 8: 'Op' }

  const alStatusCodesQuery = useQuery({
    queryKey: ['alStatusCodes'],
    queryFn: () => api.getAlStatusCodes(),
    staleTime: Infinity,
  })
  const alStatusCodeMap = Object.fromEntries(
    (alStatusCodesQuery.data?.data ?? []).map(c => [c.code, c])
  )

  type DeviceState = { alState: number; error: boolean; alStatusCode: number }
  const [deviceStates, setDeviceStates] = useState<Record<number, DeviceState>>({})
  const [readingStates, setReadingStates] = useState(false)

  async function readStatesFor(positions: number[]) {
    if (positions.length === 0) {
      setDeviceStates({})
      return
    }
    setReadingStates(true)
    try {
      const res = await api.getDeviceStates({ positions: positions.join(',') })
      const next: Record<number, DeviceState> = {}
      for (const entry of res.data) {
        next[entry.slavePosition] = { alState: entry.alState, error: entry.error, alStatusCode: entry.alStatusCode }
      }
      setDeviceStates(next)
    } finally {
      setReadingStates(false)
    }
  }

  // Non-destructive refresh: re-read the device list and current AL states.
  // Unlike Scan (ecx_config_init) this issues no bus reconfiguration, so slaves
  // keep their state (e.g. PRE-OP / OP). States are read off the freshly
  // re-fetched list so a refresh right after a scan reflects the new devices.
  async function refreshDevices() {
    const res = await devicesQuery.refetch()
    await readStatesFor(res.data?.data.map(d => d.slavePosition) ?? [])
    // Keep the sidebar's shared queries in sync with fieldbus actions
    // (scan / transition / manual refresh): AL state and the per-device online probe,
    // since a transition to/from INIT changes whether the mailbox is available.
    void queryClient.invalidateQueries({ queryKey: ['deviceStates'] })
    void queryClient.invalidateQueries({ queryKey: ['deviceOnline'] })
  }

  const initMutation = useMutation({
    mutationFn: () => api.init({ driver, adapter }),
    onSuccess: () => {
      setAlreadyInitialized(false)
      setIsInitialized(true)
    },
    onError: (err) => {
      // 409 = already initialized on the server; surface it as info, not an error.
      if (apiStatus(err) === 409) {
        setAlreadyInitialized(true)
        setIsInitialized(true)
      }
    },
  })

  const resetMutation = useMutation({
    mutationFn: () => api.reset(),
    onSuccess: () => {
      setHasScanned(false)
      setIsInitialized(false)
      setAlreadyInitialized(false)
      setDeviceStates({})
      queryClient.removeQueries({ queryKey: ['devices'] })
    },
  })

  const scanMutation = useMutation({
    mutationFn: () => api.scan(),
    onSuccess: () => {
      setHasScanned(true)
      // Pull the freshly discovered devices and their actual (post-scan: INIT)
      // states so the table reflects reality without a manual Refresh.
      return refreshDevices()
    },
  })

  const devicesQuery = useQuery({
    queryKey: ['devices'],
    queryFn: () => api.getDevices(),
    enabled: hasScanned,
  })

  const adaptersQuery = useQuery({
    queryKey: ['adapters'],
    queryFn: () => api.getAdapters(),
    enabled: false,
  })

  const transitionMutation = useMutation({
    mutationFn: () => api.transitionToState({ state: alState }),
    // The transition blocks server-side until devices arrive (or time out), so
    // re-read states afterwards to show where each device actually landed.
    onSuccess: () => refreshDevices(),
  })

  // When the device list first appears without us having read states yet — a page
  // refresh that restored the session, or navigating back to the fieldbus page — read
  // the actual AL states once so the table isn't blank until a manual refresh.
  // Scan/transition set states themselves, so this only fills the initial gap.
  const autoReadStates = useRef(false)
  useEffect(() => {
    if (autoReadStates.current) return
    if (devicesQuery.isSuccess && devicesQuery.data.data.length > 0) {
      autoReadStates.current = true
      void readStatesFor(devicesQuery.data.data.map(d => d.slavePosition))
    }
  }, [devicesQuery.isSuccess, devicesQuery.data]) // eslint-disable-line react-hooks/exhaustive-deps

  return (
    <div>
      <PageHeader
        eyebrow="App"
        title="Fieldbus"
        description="Initialize the EtherCAT fieldbus, scan for slave devices, and command AL state transitions across the bus."
      />
      <div className="p-4 sm:p-8 space-y-8">
        {/* Row 1 — Init + Reset */}
        <section>
          <div className="grid grid-cols-1 xl:grid-cols-4 gap-6">

            {/* Init */}
            <div className="border border-grey-200 p-5 space-y-4 xl:col-span-3">
              <h3 className="text-sm font-display uppercase tracking-widest">Init</h3>
              <p className="text-xs text-grey-600">
                Initialize the fieldbus driver with the selected protocol and network adapter. Must be called before scanning.
              </p>
              <div className="space-y-3">
                <div>
                  <label className={labelCls}>Driver</label>
                  <select
                    value={driver}
                    onChange={e => setDriver(e.target.value as 'soem' | 'spoe' | 'igh')}
                    className={inputCls}
                  >
                    <option value="soem">SOEM (Simple Open EtherCAT Master)</option>
                    <option value="spoe">SPoE (SOMANET Protocol over Ethernet)</option>
                    <option value="igh">IgH EtherCAT Master</option>
                  </select>
                </div>
                <div>
                  <label className={labelCls}>Adapter</label>
                  <input
                    type="text"
                    value={adapter}
                    onChange={e => setAdapter(e.target.value)}
                    placeholder="network adapter name or MAC address"
                    className={inputCls}
                  />
                  <button
                    onClick={() => adaptersQuery.refetch()}
                    disabled={adaptersQuery.isFetching}
                    className={`${btnOutlineCls} w-full mt-2`}
                  >
                    {adaptersQuery.isFetching ? 'Reading…' : 'Read Adapters'}
                  </button>
                  {adaptersQuery.isSuccess && adaptersQuery.data.data.length === 0 && (
                    <p className="text-xs text-grey-600 mt-2">No adapters found.</p>
                  )}
                  {adaptersQuery.isSuccess && adaptersQuery.data.data.length > 0 && (
                    <ul className="border border-grey-200 divide-y divide-grey-100 mt-2">
                      {adaptersQuery.data.data.map(a => (
                        <li key={a.mac}>
                          <button
                            onClick={() => setAdapter(a.name)}
                            className="w-full text-left px-3 py-2 text-xs hover:bg-grey-50 transition-colors"
                          >
                            <span>{a.name}</span>
                            <span className="text-grey-500 ml-2 font-mono">{a.mac}</span>
                          </button>
                        </li>
                      ))}
                    </ul>
                  )}
                </div>
              </div>
              <button
                onClick={() => initMutation.mutate()}
                disabled={initMutation.isPending || !adapter.trim()}
                className={btnCls}
              >
                {initMutation.isPending ? 'Initializing…' : 'Init'}
              </button>
              {!adapter.trim() && (
                <p className="text-grey-500 text-xs">Select a network adapter to initialize.</p>
              )}
              {initMutation.isSuccess && (
                <p className="text-status-good text-xs">Initialized</p>
              )}
              {initMutation.isError && apiStatus(initMutation.error) !== 409 && (
                <p className="text-status-bad text-xs">{apiError(initMutation.error)}</p>
              )}
              {alreadyInitialized && !initMutation.isSuccess && (
                <div className="border-l-2 border-status-info bg-status-info/10 px-3 py-2">
                  <p className="text-xs font-display font-medium uppercase tracking-wide text-status-info">
                    Fieldbus already initialized
                  </p>
                  <p className="text-xs text-grey-700 mt-0.5">
                    Reusing the existing session. Reset first to re-initialize.
                  </p>
                </div>
              )}
            </div>

            {/* Reset */}
            <div className="border border-grey-200 p-5 space-y-4 xl:col-span-1">
              <h3 className="text-sm font-display uppercase tracking-widest">Reset</h3>
              <p className="text-xs text-grey-600">
                Reset the fieldbus driver and clear the device list. Init must be performed again afterwards.
              </p>
              <button
                onClick={() => resetMutation.mutate()}
                disabled={resetMutation.isPending}
                className={btnCls}
              >
                {resetMutation.isPending ? 'Resetting…' : 'Reset'}
              </button>
              {resetMutation.isSuccess && (
                <p className="text-status-good text-xs">Reset</p>
              )}
              {resetMutation.isError && (
                <p className="text-status-bad text-xs">{apiError(resetMutation.error)}</p>
              )}
            </div>

          </div>
        </section>

        {/* Row 2 — Scan + Transition to State */}
        <section>
          <div className="grid grid-cols-1 xl:grid-cols-4 gap-6">

            {/* Scan + Devices */}
            <div className="border border-grey-200 p-5 space-y-4 xl:col-span-3">
              <h3 className="text-sm font-display uppercase tracking-widest">Scan</h3>
              <p className="text-xs text-grey-600">
                Discover EtherCAT slaves on the bus. Requires a successful init first.
                Re-discovers the bus and resets all slaves to INIT — use Refresh below to
                update the list and states without disturbing slave state.
              </p>
              <button
                onClick={() => scanMutation.mutate()}
                disabled={scanMutation.isPending}
                className={btnCls}
              >
                {scanMutation.isPending ? 'Scanning…' : 'Scan'}
              </button>
              {scanMutation.isSuccess && (
                <p className="text-status-good text-xs">
                  {scanMutation.data.data.slaves}{' '}
                  {scanMutation.data.data.slaves === 1 ? 'slave' : 'slaves'} discovered
                </p>
              )}
              {scanMutation.isError && (
                <p className="text-status-bad text-xs">{apiError(scanMutation.error)}</p>
              )}

              {hasScanned && (
                <div className="pt-2 space-y-2">
                  <div className="flex items-center justify-between">
                    <p className="eyebrow text-xs">Devices</p>
                    <button
                      onClick={refreshDevices}
                      disabled={devicesQuery.isFetching || readingStates}
                      className={btnOutlineCls}
                      title="Re-read the device list and AL states without re-scanning the bus — slaves keep their current state."
                    >
                      {devicesQuery.isFetching || readingStates ? 'Refreshing…' : 'Refresh'}
                    </button>
                  </div>
                  <div className="border border-grey-200 overflow-x-auto">
                    {devicesQuery.isFetching && !devicesQuery.data && (
                      <p className="p-4 text-xs text-grey-600">Loading devices…</p>
                    )}
                    {devicesQuery.isSuccess && devicesQuery.data.data.length === 0 && (
                      <p className="p-4 text-xs text-grey-600">No devices found.</p>
                    )}
                    {devicesQuery.isSuccess && devicesQuery.data.data.length > 0 && (
                      <table className="w-full text-xs border-collapse">
                        <thead>
                          <tr className="border-b border-grey-200 bg-grey-50">
                            {['Pos', 'Name', 'Vendor ID', 'Product Code', 'Revision', 'Serial', 'AL State'].map(h => (
                              <th key={h} className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium">
                                {h}
                              </th>
                            ))}
                          </tr>
                        </thead>
                        <tbody>
                          {devicesQuery.data.data.map(d => (
                            <tr key={d.slavePosition} className="border-b border-grey-100 last:border-0">
                              <td className="px-4 py-2">{d.slavePosition}</td>
                              <td className="px-4 py-2">{d.name}</td>
                              <td className="px-4 py-2 font-mono">0x{d.vendorId.toString(16).toUpperCase()}</td>
                              <td className="px-4 py-2 font-mono">0x{d.productCode.toString(16).toUpperCase()}</td>
                              <td className="px-4 py-2 font-mono">0x{d.revisionNumber.toString(16).toUpperCase()}</td>
                              <td className="px-4 py-2 font-mono">{d.serialNumber}</td>
                              <td className="px-4 py-2">
                                {deviceStates[d.slavePosition] !== undefined
                                  ? (() => {
                                    const ds = deviceStates[d.slavePosition]
                                    const stateLabel = AL_STATE_LABEL[ds.alState] ?? `0x${ds.alState.toString(16).toUpperCase()}`
                                    const errorEntry = ds.error ? (alStatusCodeMap[ds.alStatusCode] ?? null) : null
                                    return (
                                      <span className={ds.error ? 'text-status-bad' : ''}>
                                        {stateLabel} ({ds.alState})
                                        {errorEntry && (
                                          <span title={errorEntry.description}>
                                            {' — '}0x{ds.alStatusCode.toString(16).toUpperCase().padStart(4, '0')} {errorEntry.name}
                                            {errorEntry.terminal && (
                                              <span
                                                className="ml-1 text-grey-600"
                                                title="Terminal: the slave cannot reach the requested state by retrying. Re-init, reflash, or power cycle is required."
                                              >
                                                (terminal)
                                              </span>
                                            )}
                                          </span>
                                        )}
                                        {ds.error && !errorEntry && (
                                          <span>{' — '}0x{ds.alStatusCode.toString(16).toUpperCase().padStart(4, '0')}</span>
                                        )}
                                      </span>
                                    )
                                  })()
                                  : '—'}
                              </td>
                            </tr>
                          ))}
                        </tbody>
                      </table>
                    )}
                  </div>
                </div>
              )}
            </div>

            {/* Transition to State */}
            <div className="border border-grey-200 p-5 space-y-4 xl:col-span-1">
              <h3 className="text-sm font-display uppercase tracking-widest">Transition to State</h3>
              <p className="text-xs text-grey-600">
                Command all slaves to transition to the selected EtherCAT AL state. Requires a successful scan first.
              </p>
              <div>
                <label className={labelCls}>Target state</label>
                <select
                  value={alState}
                  onChange={e => setAlState(Number(e.target.value) as 1 | 2 | 3 | 4 | 8)}
                  className={inputCls}
                >
                  {AL_STATES.map(({ value, label }) => (
                    <option key={value} value={value}>{label} ({value})</option>
                  ))}
                </select>
              </div>
              <button
                onClick={() => transitionMutation.mutate()}
                disabled={transitionMutation.isPending}
                className={btnCls}
              >
                {transitionMutation.isPending ? 'Transitioning…' : 'Transition'}
              </button>
              {transitionMutation.isSuccess && (
                <p className="text-status-good text-xs">Transitioned</p>
              )}
              {transitionMutation.isError && (
                <p className="text-status-bad text-xs">{apiError(transitionMutation.error)}</p>
              )}
            </div>

          </div>
        </section>

      </div>
    </div>
  )
}
