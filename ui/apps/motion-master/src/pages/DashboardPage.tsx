import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import PageHeader from '../components/PageHeader'
import { api } from '../api'

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

const inputCls = 'border border-grey-300 px-3 py-2 text-sm w-full bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'
const btnCls =
  'bg-syn-red text-white px-4 py-2 text-xs hover:bg-ocean disabled:opacity-50 w-full transition-colors'
const btnOutlineCls =
  'border border-syn-red text-syn-red px-3 py-1.5 text-xs hover:bg-syn-red hover:text-white disabled:opacity-50 transition-colors'

export default function DashboardPage() {
  const queryClient = useQueryClient()
  const [driver, setDriver] = useState<'soem' | 'spoe' | 'igh'>('soem')
  const [adapter, setAdapter] = useState('')
  const [alState, setAlState] = useState<1 | 2 | 3 | 4 | 8>(8)
  const [hasScanned, setHasScanned] = useState(false)

  const initMutation = useMutation({
    mutationFn: () => api.init({ driver, adapter }),
  })

  const scanMutation = useMutation({
    mutationFn: () => api.scan(),
    onSuccess: () => {
      setHasScanned(true)
      queryClient.invalidateQueries({ queryKey: ['devices'] })
    },
  })

  const devicesQuery = useQuery({
    queryKey: ['devices'],
    queryFn: () => api.getDevices(),
    enabled: hasScanned,
  })

  const transitionMutation = useMutation({
    mutationFn: () => api.transitionToState({ state: alState }),
  })

  return (
    <div>
      <PageHeader eyebrow="App" title="Dashboard" />
      <div className="p-8 space-y-8">
        <p className="text-sm">
          <a
            href="/docs"
            target="_blank"
            rel="noopener noreferrer"
            className="text-syn-red underline hover:opacity-70"
          >
            Documentation
          </a>
        </p>

        {/* Fieldbus — numbered steps */}
        <section>
          <p className="eyebrow mb-5">Fieldbus</p>
          <div className="grid grid-cols-3 gap-6">

            {/* 1. Init */}
            <div className="border border-grey-200 p-5 space-y-4">
              <h3 className="text-sm font-display uppercase tracking-widest">
                <span className="text-syn-red mr-1">1.</span>Init
              </h3>
              <div className="space-y-3">
                <div>
                  <label className={labelCls}>Driver</label>
                  <select
                    value={driver}
                    onChange={e => setDriver(e.target.value as 'soem' | 'spoe' | 'igh')}
                    className={inputCls}
                  >
                    <option value="soem">SOEM</option>
                    <option value="spoe">SPoE</option>
                    <option value="igh">IgH</option>
                  </select>
                </div>
                <div>
                  <label className={labelCls}>Adapter</label>
                  <input
                    type="text"
                    value={adapter}
                    onChange={e => setAdapter(e.target.value)}
                    placeholder="auto-detect"
                    className={inputCls}
                  />
                </div>
              </div>
              <button
                onClick={() => initMutation.mutate()}
                disabled={initMutation.isPending}
                className={btnCls}
              >
                {initMutation.isPending ? 'Initializing…' : 'Init'}
              </button>
              {initMutation.isSuccess && (
                <p className="text-status-good text-xs">Initialized</p>
              )}
              {initMutation.isError && (
                <p className="text-status-bad text-xs">{apiError(initMutation.error)}</p>
              )}
            </div>

            {/* 2. Scan */}
            <div className="border border-grey-200 p-5 space-y-4">
              <h3 className="text-sm font-display uppercase tracking-widest">
                <span className="text-syn-red mr-1">2.</span>Scan
              </h3>
              <p className="text-xs text-grey-600">
                Discover EtherCAT slaves on the bus. Requires a successful init first.
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
            </div>

            {/* 3. Transition to State */}
            <div className="border border-grey-200 p-5 space-y-4">
              <h3 className="text-sm font-display uppercase tracking-widest">
                <span className="text-syn-red mr-1">3.</span>Transition to State
              </h3>
              <div>
                <label className={labelCls}>Target state</label>
                <select
                  value={alState}
                  onChange={e => setAlState(Number(e.target.value) as 1 | 2 | 3 | 4 | 8)}
                  className={inputCls}
                >
                  {AL_STATES.map(({ value, label }) => (
                    <option key={value} value={value}>{label}</option>
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

        {/* Devices — separate section, populated after scan */}
        {hasScanned && (
          <section>
            <div className="flex items-center justify-between mb-4">
              <p className="eyebrow">Devices</p>
              <button
                onClick={() => devicesQuery.refetch()}
                disabled={devicesQuery.isFetching}
                className={btnOutlineCls}
              >
                {devicesQuery.isFetching ? 'Reading…' : 'Re-read'}
              </button>
            </div>
            <div className="border border-grey-200">
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
                      {['Pos', 'Name', 'Vendor ID', 'Product Code', 'Revision', 'Serial'].map(h => (
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
                      </tr>
                    ))}
                  </tbody>
                </table>
              )}
            </div>
          </section>
        )}
      </div>
    </div>
  )
}
