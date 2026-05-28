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

  const paramsQueryKey = ['deviceParameters', slavePosition] as const

  const paramsQuery = useQuery({
    queryKey: paramsQueryKey,
    queryFn: () => api.getDeviceParameters(slavePosition),
    staleTime: Infinity,
    retry: false,
  })

  const initMutation = useMutation({
    mutationFn: () => api.initializeDeviceParameters(slavePosition, { readValues }),
    onSuccess: (res) => {
      queryClient.setQueryData(paramsQueryKey, res)
    },
  })

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
          <p className="eyebrow mb-5">Parameter list</p>
          <div className="border border-grey-200 p-5 space-y-4">
            <div className="flex flex-wrap items-end gap-3">
              <label className="flex items-center gap-2 text-xs text-grey-700 select-none cursor-pointer">
                <input
                  type="checkbox"
                  checked={readValues}
                  onChange={e => setReadValues(e.target.checked)}
                  className="cursor-pointer"
                />
                <span>Read values during init (SDO upload per entry — slower)</span>
              </label>
              <div className="flex gap-2 ml-auto">
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
                  onClick={() => paramsQuery.refetch()}
                  title="Re-fetch the cached parameter list (no SDO Info traffic). Use after values may have changed on the device."
                >
                  {paramsQuery.isFetching ? 'Reloading…' : 'Reload'}
                </button>
              </div>
            </div>

            {initError && (
              <p className="text-xs text-status-bad font-mono">{initError}</p>
            )}

            {params.length > 0 && (
              <>
                <div className="flex items-center gap-3">
                  <input
                    type="text"
                    value={filter}
                    onChange={e => setFilter(e.target.value)}
                    placeholder="Filter by name, index, or type…"
                    className={`${inputCls} max-w-sm`}
                  />
                  <span className="text-xs text-grey-500">
                    {filteredParams.length === params.length
                      ? `${params.length} entries`
                      : `${filteredParams.length} / ${params.length}`}
                  </span>
                </div>

                <div className="border border-grey-200 overflow-x-auto">
                  <table className="w-full text-xs border-collapse">
                    <thead>
                      <tr className="border-b border-grey-200 bg-grey-50">
                        {['Address', 'Name', 'Type', 'Bits', 'Access', 'Value', 'Default', 'Min', 'Max', 'Unit'].map(h => (
                          <th key={h} className="text-left px-3 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">
                            {h}
                          </th>
                        ))}
                      </tr>
                    </thead>
                    <tbody>
                      {filteredParams.map(p => (
                        <tr key={`${p.index}-${p.subindex}`} className="border-b border-grey-100 last:border-0">
                          <td className="px-3 py-1.5 font-mono whitespace-nowrap">
                            {toHex(p.index, 4)}:{p.subindex.toString(16).toUpperCase().padStart(2, '0')}
                          </td>
                          <td className="px-3 py-1.5 text-grey-800">{p.name || <span className="text-grey-400">—</span>}</td>
                          <td className="px-3 py-1.5 font-mono text-grey-700">{p.dataTypeName}</td>
                          <td className="px-3 py-1.5 font-mono text-grey-700">{p.bitLength}</td>
                          <td className="px-3 py-1.5 font-mono text-grey-700">{formatAccess(p.access)}</td>
                          <td className="px-3 py-1.5 font-mono">{formatValue(p.value)}</td>
                          <td className="px-3 py-1.5 font-mono text-grey-600">{formatValue(p.defaultValue)}</td>
                          <td className="px-3 py-1.5 font-mono text-grey-600">{formatValue(p.minValue)}</td>
                          <td className="px-3 py-1.5 font-mono text-grey-600">{formatValue(p.maxValue)}</td>
                          <td className="px-3 py-1.5 font-mono text-grey-600">{p.unit !== undefined ? toHex(p.unit, 8) : '—'}</td>
                        </tr>
                      ))}
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

      </div>
    </div>
  )
}
