import { useState } from 'react'
import { useParams } from 'react-router'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { DeviceParameter } from '@mm/api-client'
import DevicePageHeader from '../components/DevicePageHeader'
import { useConnection } from '../contexts/ConnectionContext'

const inputCls = 'border border-grey-300 px-3 py-2 text-sm w-full bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'
const btnCls =
  'bg-syn-red text-white px-4 py-2 text-xs hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'
const btnGhostCls =
  'border border-grey-300 text-grey-700 px-4 py-2 text-xs hover:bg-grey-50 disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

function apiError(err: unknown): string {
  if (err && typeof err === 'object' && 'error' in err) {
    const inner = (err as { error: unknown }).error
    if (inner && typeof inner === 'object' && 'error' in inner) {
      return String((inner as { error: unknown }).error)
    }
  }
  return 'Unknown error'
}

type Interpretation = { label: string; value: string }

function interpretBytes(bytes: number[]): Interpretation[] {
  const view = new DataView(new Uint8Array(bytes).buffer)
  const out: Interpretation[] = []

  if (bytes.length === 1) {
    out.push({ label: 'int8',  value: String(view.getInt8(0)) })
    out.push({ label: 'uint8', value: String(view.getUint8(0)) })
  } else if (bytes.length === 2) {
    out.push({ label: 'int16 LE',  value: String(view.getInt16(0, true)) })
    out.push({ label: 'uint16 LE', value: String(view.getUint16(0, true)) })
  } else if (bytes.length === 4) {
    out.push({ label: 'int32 LE',   value: String(view.getInt32(0, true)) })
    out.push({ label: 'uint32 LE',  value: String(view.getUint32(0, true)) })
    out.push({ label: 'float32 LE', value: String(view.getFloat32(0, true)) })
  } else if (bytes.length === 8) {
    out.push({ label: 'int64 LE',   value: view.getBigInt64(0, true).toString() })
    out.push({ label: 'uint64 LE',  value: view.getBigUint64(0, true).toString() })
    out.push({ label: 'float64 LE', value: String(view.getFloat64(0, true)) })
  }

  if (bytes.length > 0 && bytes.every(b => (b >= 0x20 && b <= 0x7e) || b === 0)) {
    const str = new TextDecoder().decode(new Uint8Array(bytes.filter(b => b !== 0)))
    out.push({ label: 'string', value: `"${str}"` })
  }

  return out
}

function parseHexOrDec(s: string): number | null {
  const t = s.trim()
  if (/^0[xX][0-9a-fA-F]+$/.test(t)) return parseInt(t.slice(2), 16)
  if (/^[0-9]+$/.test(t)) return parseInt(t, 10)
  return null
}

function toHex(n: number, pad: number): string {
  return `0x${n.toString(16).toUpperCase().padStart(pad, '0')}`
}

function formatValue(v: number | string | number[] | undefined): string {
  if (v === undefined) return '—'
  if (typeof v === 'number') return String(v)
  if (typeof v === 'string') return v === '' ? '""' : `"${v}"`
  // byte array — render up to 16 bytes in hex
  if (v.length === 0) return '[]'
  const head = v.slice(0, 16).map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ')
  return v.length > 16 ? `${head} …(${v.length}B)` : head
}

function formatAccess(a: number): string {
  // ETG.1000.6: bit 0=Rd PreOp, 1=Rd SafeOp, 2=Rd Op, 3=Wr PreOp, 4=Wr SafeOp, 5=Wr Op
  const r = (a & 0x07) !== 0
  const w = (a & 0x38) !== 0
  if (r && w) return 'RW'
  if (r) return 'RO'
  if (w) return 'WO'
  return '—'
}

function formatElapsed(ms: number): string {
  if (ms < 1000) return `${Math.round(ms)} ms`
  return `${(ms / 1000).toFixed(2)} s`
}

function decodeSdoBytes(dataTypeName: string, bytes: number[]): number | string | number[] {
  const view = new DataView(new Uint8Array(bytes).buffer)
  switch (dataTypeName) {
    case 'BOOLEAN':
    case 'UNSIGNED8':
    case 'BYTE':
      if (bytes.length >= 1) return view.getUint8(0)
      break
    case 'INTEGER8':
      if (bytes.length >= 1) return view.getInt8(0)
      break
    case 'INTEGER16':
      if (bytes.length >= 2) return view.getInt16(0, true)
      break
    case 'INTEGER32':
      if (bytes.length >= 4) return view.getInt32(0, true)
      break
    case 'UNSIGNED16':
    case 'WORD':
      if (bytes.length >= 2) return view.getUint16(0, true)
      break
    case 'UNSIGNED32':
    case 'DWORD':
      if (bytes.length >= 4) return view.getUint32(0, true)
      break
    case 'REAL32':
      if (bytes.length >= 4) return view.getFloat32(0, true)
      break
    case 'REAL64':
      if (bytes.length >= 8) return view.getFloat64(0, true)
      break
    case 'INTEGER64':
      if (bytes.length >= 8) return Number(view.getBigInt64(0, true))
      break
    case 'UNSIGNED64':
      if (bytes.length >= 8) return Number(view.getBigUint64(0, true))
      break
    case 'VISIBLE_STRING':
    case 'UNICODE_STRING':
      return new TextDecoder().decode(new Uint8Array(bytes.filter(b => b !== 0)))
  }
  return bytes
}

function paramKey(index: number, subindex: number): string {
  return `${index}-${subindex}`
}

// Freshness of the cached value relative to the device, rendered as a labelled badge.
// Each state carries a title explaining it; the help cursor signals the tooltip.
function SyncBadge({ state }: { state: DeviceParameter['syncState'] }) {
  if (state === 'synced') {
    return (
      <span
        title="Synced — value matches the device (last successful read or write)"
        className="inline-block px-1.5 py-0.5 rounded-sm text-[10px] uppercase tracking-wide font-display cursor-help bg-status-good/10 text-status-good"
      >
        Synced
      </span>
    )
  }
  if (state === 'pending') {
    return (
      <span
        title="Pending — set locally while offline or after a failed write, not yet confirmed on the device"
        className="inline-block px-1.5 py-0.5 rounded-sm text-[10px] uppercase tracking-wide font-display cursor-help bg-status-warn/25 text-grey-800"
      >
        Pending
      </span>
    )
  }
  return (
    <span
      title="Unknown — never read; value is the type-appropriate default"
      className="inline-block px-1.5 py-0.5 rounded-sm text-[10px] uppercase tracking-wide font-display cursor-help bg-grey-100 text-grey-500"
    >
      Unknown
    </span>
  )
}

export default function ParametersPage() {
  const { deviceId } = useParams()
  const { api } = useConnection()
  const slavePosition = Number(deviceId)
  const queryClient = useQueryClient()

  const [index, setIndex] = useState('')
  const [subindex, setSubindex] = useState('0')
  const [uploading, setUploading] = useState(false)
  const [result, setResult] = useState<number[] | null>(null)
  const [error, setError] = useState<string | null>(null)

  const [readValues, setReadValues] = useState(false)
  const [filter, setFilter] = useState('')
  const [initElapsedMs, setInitElapsedMs] = useState<number | null>(null)
  const [reloadElapsedMs, setReloadElapsedMs] = useState<number | null>(null)
  const [refreshingKeys, setRefreshingKeys] = useState<Set<string>>(new Set())
  const [rowError, setRowError] = useState<{key: string; message: string} | null>(null)

  const paramsQueryKey = ['deviceParameters', slavePosition] as const

  const paramsQuery = useQuery({
    queryKey: paramsQueryKey,
    queryFn: () => api.getDeviceParameters(slavePosition),
    staleTime: Infinity,
    retry: false,
  })

  const initMutation = useMutation({
    mutationFn: async () => {
      const start = performance.now()
      const res = await api.initializeDeviceParameters(slavePosition, { readValues })
      setInitElapsedMs(performance.now() - start)
      return res
    },
    onMutate: () => {
      setInitElapsedMs(null)
      setReloadElapsedMs(null)
    },
    onSuccess: (res) => {
      queryClient.setQueryData(paramsQueryKey, res)
    },
  })

  async function handleReload() {
    setReloadElapsedMs(null)
    setInitElapsedMs(null)
    const start = performance.now()
    await paramsQuery.refetch()
    setReloadElapsedMs(performance.now() - start)
  }

  async function handleRefreshValue(p: DeviceParameter) {
    const key = paramKey(p.index, p.subindex)
    setRefreshingKeys(prev => new Set(prev).add(key))
    setRowError(null)
    try {
      const res = await api.sdoUpload(slavePosition, p.index, p.subindex)
      const decoded = decodeSdoBytes(p.dataTypeName, res.data.data)
      queryClient.setQueryData(paramsQueryKey, (prev: typeof paramsQuery.data) => {
        if (!prev) return prev
        const next = prev.data.map(x =>
          x.index === p.index && x.subindex === p.subindex
            ? { ...x, value: decoded, syncState: 'synced' as const }
            : x,
        )
        return { ...prev, data: next }
      })
    } catch (err) {
      setRowError({ key, message: apiError(err) })
    } finally {
      setRefreshingKeys(prev => {
        const next = new Set(prev)
        next.delete(key)
        return next
      })
    }
  }

  const params: DeviceParameter[] = paramsQuery.data?.data ?? []
  const initError = initMutation.error ? apiError(initMutation.error) : null

  const filterLower = filter.trim().toLowerCase()
  const filteredParams = filterLower
    ? params.filter(p =>
        p.name.toLowerCase().includes(filterLower) ||
        toHex(p.index, 4).toLowerCase().includes(filterLower) ||
        p.dataTypeName.toLowerCase().includes(filterLower),
      )
    : params

  const indexNum = parseHexOrDec(index)
  const subindexNum = parseHexOrDec(subindex)

  const indexValid = indexNum !== null && indexNum >= 0 && indexNum <= 0xffff
  const subindexValid = subindexNum !== null && subindexNum >= 0 && subindexNum <= 0xff
  const canUpload = indexValid && subindexValid && !uploading

  function handleIndexChange(val: string) {
    setIndex(val)
    setResult(null)
    setError(null)
  }

  function handleSubindexChange(val: string) {
    setSubindex(val)
    setResult(null)
    setError(null)
  }

  async function handleUpload() {
    if (!canUpload) return
    setUploading(true)
    setResult(null)
    setError(null)
    try {
      const res = await api.sdoUpload(slavePosition, indexNum!, subindexNum!)
      setResult(res.data.data)
    } catch (err) {
      setError(apiError(err))
    } finally {
      setUploading(false)
    }
  }

  return (
    <div>
      <DevicePageHeader slavePosition={slavePosition} title="Parameters" />
      <div className="p-4 sm:p-8 space-y-8">

        <section>
          <p className="eyebrow mb-5">SDO Upload</p>
          <div className="border border-grey-200 p-5 max-w-xl space-y-4">

            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className={labelCls}>Index (hex or dec)</label>
                <input
                  type="text"
                  value={index}
                  onChange={e => handleIndexChange(e.target.value)}
                  placeholder="e.g. 0x6064 or 24676"
                  className={inputCls}
                />
                <p className="text-xs text-grey-500 mt-1 font-mono">
                  {indexValid ? toHex(indexNum!, 4) : '—'}
                </p>
              </div>
              <div>
                <label className={labelCls}>Subindex (hex or dec)</label>
                <input
                  type="text"
                  value={subindex}
                  onChange={e => handleSubindexChange(e.target.value)}
                  placeholder="e.g. 0x00 or 0"
                  className={inputCls}
                />
                <p className="text-xs text-grey-500 mt-1 font-mono">
                  {subindexValid ? toHex(subindexNum!, 2) : '—'}
                </p>
              </div>
            </div>

            <button onClick={handleUpload} disabled={!canUpload} className={btnCls}>
              {uploading ? 'Uploading…' : 'Upload'}
            </button>

            {error && (
              <p className="text-xs text-status-bad font-mono">{error}</p>
            )}

            {result && (
              <div className="border border-grey-200 p-3 space-y-1 bg-grey-50">
                <p className="text-xs text-grey-600 uppercase tracking-wide mb-2">Result</p>
                <p className="text-xs font-mono">
                  <span className="text-grey-600">Hex:&nbsp;</span>
                  {result.map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ')}
                </p>
                <p className="text-xs font-mono">
                  <span className="text-grey-600">Dec:&nbsp;</span>
                  [{result.join(', ')}]
                </p>
                {interpretBytes(result).map(({ label, value }) => (
                  <p key={label} className="text-xs font-mono">
                    <span className="text-grey-600">{label}:&nbsp;</span>
                    {value}
                  </p>
                ))}
              </div>
            )}

          </div>
        </section>

        <section>
          <p className="eyebrow mb-5">Parameter list</p>
          <div className="border border-grey-200 p-5 space-y-4">
            <div className="flex flex-wrap items-center gap-x-3 gap-y-2">
              {params.length > 0 && (
                <div className="flex items-center gap-3 flex-1 min-w-[12rem]">
                  <input
                    type="text"
                    value={filter}
                    onChange={e => setFilter(e.target.value)}
                    placeholder="Filter by name, index, or type…"
                    className={`${inputCls} max-w-sm`}
                  />
                  <span className="text-xs text-grey-500 whitespace-nowrap">
                    {filteredParams.length === params.length
                      ? `${params.length} entries`
                      : `${filteredParams.length} / ${params.length}`}
                  </span>
                </div>
              )}
              <div className="flex flex-wrap items-center gap-3 ml-auto">
                <label className="flex items-center gap-2 text-xs text-grey-700 select-none cursor-pointer">
                  <input
                    type="checkbox"
                    checked={readValues}
                    onChange={e => setReadValues(e.target.checked)}
                    className="cursor-pointer"
                  />
                  <span>Read values during init (SDO upload per entry — slower)</span>
                </label>
                <button
                  className={btnCls}
                  disabled={initMutation.isPending}
                  onClick={() => initMutation.mutate()}
                >
                  {initMutation.isPending ? 'Initialising…' : params.length === 0 ? 'Initialize' : 'Re-initialize'}
                </button>
                <button
                  className={btnGhostCls}
                  disabled={paramsQuery.isFetching || params.length === 0}
                  onClick={handleReload}
                  title="Re-fetch the cached parameter list (no SDO Info traffic). Use after values may have changed on the device."
                >
                  {paramsQuery.isFetching ? 'Reloading…' : 'Reload'}
                </button>
                {(initElapsedMs !== null || reloadElapsedMs !== null) && (
                  <span className="text-xs text-grey-500 font-mono whitespace-nowrap">
                    took {formatElapsed(initElapsedMs ?? reloadElapsedMs!)}
                  </span>
                )}
              </div>
            </div>

            {initError && (
              <p className="text-xs text-status-bad font-mono">{initError}</p>
            )}

            {params.length > 0 && (
              <>
                <div className="border border-grey-200 overflow-x-auto">
                  <table className="w-full text-xs border-collapse">
                    <thead>
                      <tr className="border-b border-grey-200 bg-grey-50">
                        {['Address', 'Name', 'Type', 'Bits', 'Access', 'Value', 'Sync', 'Default', 'Min', 'Max', 'Unit'].map(h => (
                          <th key={h} className="text-left px-3 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">
                            {h}
                          </th>
                        ))}
                      </tr>
                    </thead>
                    <tbody>
                      {filteredParams.map(p => {
                        const key = paramKey(p.index, p.subindex)
                        const refreshing = refreshingKeys.has(key)
                        const cellError = rowError?.key === key ? rowError.message : null
                        return (
                          <tr key={key} className="border-b border-grey-100 last:border-0">
                            <td className="px-3 py-1.5 font-mono whitespace-nowrap">
                              {toHex(p.index, 4)}:{p.subindex.toString(16).toUpperCase().padStart(2, '0')}
                            </td>
                            <td className="px-3 py-1.5 text-grey-800">{p.name || <span className="text-grey-400">—</span>}</td>
                            <td className="px-3 py-1.5 font-mono text-grey-700">{p.dataTypeName}</td>
                            <td className="px-3 py-1.5 font-mono text-grey-700">{p.bitLength}</td>
                            <td className="px-3 py-1.5 font-mono text-grey-700">{formatAccess(p.access)}</td>
                            <td className="px-3 py-1.5 font-mono">
                              <div className="flex items-center gap-2">
                                <span className={cellError ? 'text-status-bad' : ''}>
                                  {cellError ?? formatValue(p.value)}
                                </span>
                                <button
                                  type="button"
                                  onClick={() => handleRefreshValue(p)}
                                  disabled={refreshing}
                                  className="text-grey-400 hover:text-syn-red disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer leading-none"
                                  title="Refresh by SDO upload"
                                  aria-label="Refresh value"
                                >
                                  {refreshing ? '…' : '↻'}
                                </button>
                              </div>
                            </td>
                            <td className="px-3 py-1.5 whitespace-nowrap"><SyncBadge state={p.syncState} /></td>
                            <td className="px-3 py-1.5 font-mono text-grey-600">{formatValue(p.defaultValue)}</td>
                            <td className="px-3 py-1.5 font-mono text-grey-600">{formatValue(p.minValue)}</td>
                            <td className="px-3 py-1.5 font-mono text-grey-600">{formatValue(p.maxValue)}</td>
                            <td className="px-3 py-1.5 font-mono text-grey-600">{p.unit !== undefined ? toHex(p.unit, 8) : '—'}</td>
                          </tr>
                        )
                      })}
                    </tbody>
                  </table>
                </div>
              </>
            )}

            {params.length === 0 && !initMutation.isPending && (
              <p className="text-xs text-grey-500">
                No parameters loaded. Click <em>Initialize</em> to enumerate the device's object dictionary
                (requires the device to be in PRE-OP or higher).
              </p>
            )}
          </div>
        </section>

      </div>
    </div>
  )
}
