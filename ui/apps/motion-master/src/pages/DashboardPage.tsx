import { useState } from 'react'
import { useMutation } from '@tanstack/react-query'
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

export default function DashboardPage() {
  const [driver, setDriver] = useState<'soem' | 'spoe' | 'igh'>('soem')
  const [adapter, setAdapter] = useState('')
  const [alState, setAlState] = useState<1 | 2 | 3 | 4 | 8>(8)

  const initMutation = useMutation({
    mutationFn: () => api.init({ driver, adapter }),
  })

  const scanMutation = useMutation({
    mutationFn: () => api.scan(),
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

        <section>
          <p className="eyebrow mb-5">Fieldbus</p>
          <div className="grid grid-cols-3 gap-6">

            {/* Init */}
            <div className="border border-grey-200 p-5 space-y-4">
              <h3 className="text-sm font-display uppercase tracking-widest">Init</h3>
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

            {/* Scan */}
            <div className="border border-grey-200 p-5 space-y-4">
              <h3 className="text-sm font-display uppercase tracking-widest">Scan</h3>
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

            {/* Transition to State */}
            <div className="border border-grey-200 p-5 space-y-4">
              <h3 className="text-sm font-display uppercase tracking-widest">Transition to State</h3>
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
      </div>
    </div>
  )
}
