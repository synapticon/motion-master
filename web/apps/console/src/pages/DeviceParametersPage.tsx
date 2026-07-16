import { useState } from 'react'
import { useParams } from 'react-router'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import DevicePageHeader from '../components/DevicePageHeader'
import HexDecInput from '../components/HexDecInput'
import { useConnection } from '../contexts/ConnectionContext'
import {
  type DeviceParameter,
  SDO_TYPES,
  SDO_TYPE_HINT,
  type SdoType,
  encodeSdoValue,
  decodeSdoBytes,
  interpretSdoBytes,
  sdoTypeForDataTypeName,
  isIntegerSdoType,
} from '@synapticon/motion-master-client'

const inputCls = 'border border-grey-300 px-3 py-2 text-sm w-full bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'
const btnCls =
  'bg-syn-red text-white px-4 py-2 text-xs hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'
const btnGhostCls =
  'border border-grey-300 text-grey-700 px-4 py-2 text-xs hover:bg-grey-50 disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

function apiError(err: unknown): string {
  if (err && typeof err === 'object') {
    if ('error' in err) {
      const inner = (err as { error: unknown }).error
      // Structured driver error: { error: { error: "message" } }
      if (inner && typeof inner === 'object' && 'error' in inner) {
        return String((inner as { error: unknown }).error)
      }
      if (typeof inner === 'string') return inner
    }
    // Non-OK response with no JSON error body (e.g. an unmatched route → 404).
    // Surface the HTTP status so the cause is diagnosable instead of opaque.
    if ('status' in err && typeof (err as { status: unknown }).status === 'number') {
      const { status } = err as { status: number }
      const statusText =
        'statusText' in err ? String((err as { statusText: unknown }).statusText) : ''
      return `HTTP ${status}${statusText ? ` ${statusText}` : ''}`
    }
  }
  return 'Unknown error'
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

// The current cached value rendered for an editable text input: plain (unquoted)
// for numbers/strings, space-separated hex for byte arrays (matching `raw` input).
function valueToInputString(v: number | string | number[] | undefined): string {
  if (v === undefined) return ''
  if (typeof v === 'number') return String(v)
  if (typeof v === 'string') return v
  return v.map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ')
}

// ETG.1000.6 ObjAccess: write bits 3-5 (Wr PreOp/SafeOp/Op). Writable iff any is set.
function isWritable(access: number): boolean {
  return (access & 0x38) !== 0
}

// ETG.1000.6 ObjAccess: read bits 0-2 (Rd PreOp/SafeOp/Op). Readable iff any is set.
function isReadable(access: number): boolean {
  return (access & 0x07) !== 0
}

// Compares two decoded parameter values. Both come through decodeSdoBytes, so a
// written/read-back pair with identical wire bytes compares equal even for floats
// (same bytes → same decoded number); strings ignore NUL padding (already stripped).
function valuesEqual(
  a: number | string | number[] | undefined,
  b: number | string | number[] | undefined,
): boolean {
  if (Array.isArray(a) && Array.isArray(b)) {
    return a.length === b.length && a.every((x, i) => x === b[i])
  }
  return a === b
}

function formatAccess(a: number): string {
  // ETG.1000.6: bit 0=Rd PreOp, 1=Rd SafeOp, 2=Rd Op, 3=Wr PreOp, 4=Wr SafeOp, 5=Wr Op
  const r = (a & 0x07) !== 0
  const w = isWritable(a)
  if (r && w) return 'RW'
  if (r) return 'RO'
  if (w) return 'WO'
  return '—'
}

function formatElapsed(ms: number): string {
  if (ms < 1000) return `${Math.round(ms)} ms`
  return `${(ms / 1000).toFixed(2)} s`
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

export default function DeviceParametersPage() {
  const { deviceId } = useParams()
  const { api } = useConnection()
  const slavePosition = Number(deviceId)
  const queryClient = useQueryClient()

  const [index, setIndex] = useState('')
  const [subindex, setSubindex] = useState('0')
  const [uploading, setUploading] = useState(false)
  const [result, setResult] = useState<number[] | null>(null)
  const [error, setError] = useState<string | null>(null)

  const [dlIndex, setDlIndex] = useState('')
  const [dlSubindex, setDlSubindex] = useState('0')
  const [dlType, setDlType] = useState<SdoType>('uint32')
  const [dlValue, setDlValue] = useState('')
  const [downloading, setDownloading] = useState(false)
  const [dlOk, setDlOk] = useState<number[] | null>(null)
  const [dlError, setDlError] = useState<string | null>(null)

  const [readValues, setReadValues] = useState(false)
  const [filter, setFilter] = useState('')
  const [initElapsedMs, setInitElapsedMs] = useState<number | null>(null)
  const [readAllElapsedMs, setReadAllElapsedMs] = useState<number | null>(null)
  const [reloadElapsedMs, setReloadElapsedMs] = useState<number | null>(null)
  const [refreshingKeys, setRefreshingKeys] = useState<Set<string>>(new Set())
  const [settingKeys, setSettingKeys] = useState<Set<string>>(new Set())
  // Per-row in-progress edits, keyed by index-subindex. Absent means the input
  // mirrors the cached value; present means the user has typed an override.
  const [editValues, setEditValues] = useState<Record<string, string>>({})
  // A single transient message attached to one row: an 'error' (red, the op
  // failed) or a 'note' (amber, the op succeeded but the result is unexpected,
  // e.g. the device clamped a write).
  const [rowMsg, setRowMsg] =
    useState<{ key: string; message: string; kind: 'error' | 'note' } | null>(null)

  const paramsQueryKey = ['deviceParameters', slavePosition] as const

  const paramsQuery = useQuery({
    queryKey: paramsQueryKey,
    queryFn: () => api.getDeviceParameters(slavePosition),
    staleTime: Infinity,
    retry: false,
    // The OD is read automatically when the device reaches PRE-OP, which can land after this
    // query's first (empty) fetch; staleTime: Infinity would otherwise pin that empty list. Poll
    // while empty so the list appears once the backend has enumerated it, then stop.
    refetchInterval: (query) => ((query.state.data?.data?.length ?? 0) > 0 ? false : 2000),
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
      setReadAllElapsedMs(null)
      setReloadElapsedMs(null)
    },
    onSuccess: (res) => {
      queryClient.setQueryData(paramsQueryKey, res)
    },
  })

  const readAllMutation = useMutation({
    mutationFn: async () => {
      const start = performance.now()
      const res = await api.readAllDeviceParameters(slavePosition)
      setReadAllElapsedMs(performance.now() - start)
      return res
    },
    onMutate: () => {
      setInitElapsedMs(null)
      setReadAllElapsedMs(null)
      setReloadElapsedMs(null)
    },
    onSuccess: (res) => {
      queryClient.setQueryData(paramsQueryKey, res)
    },
  })

  async function handleReload() {
    setReloadElapsedMs(null)
    setReadAllElapsedMs(null)
    setInitElapsedMs(null)
    const start = performance.now()
    await paramsQuery.refetch()
    setReloadElapsedMs(performance.now() - start)
  }

  async function handleRefreshValue(p: DeviceParameter) {
    const key = paramKey(p.index, p.subindex)
    setRefreshingKeys(prev => new Set(prev).add(key))
    setRowMsg(null)
    try {
      // Smart read: PDO-aware (live process image when exchanging, SDO otherwise) and returns the
      // full parameter (decoded value + sync state), so no client-side byte decode is needed.
      const res = await api.readParameter(slavePosition, p.index, p.subindex)
      queryClient.setQueryData(paramsQueryKey, (prev: typeof paramsQuery.data) => {
        if (!prev) return prev
        const next = prev.data.map(x =>
          x.index === p.index && x.subindex === p.subindex ? res.data : x,
        )
        return { ...prev, data: next }
      })
      clearEdit(key)
    } catch (err) {
      setRowMsg({ key, message: apiError(err), kind: 'error' })
    } finally {
      setRefreshingKeys(prev => {
        const next = new Set(prev)
        next.delete(key)
        return next
      })
    }
  }

  function clearEdit(key: string) {
    setEditValues(prev => {
      if (!(key in prev)) return prev
      const next = { ...prev }
      delete next[key]
      return next
    })
  }

  async function handleSetValue(p: DeviceParameter) {
    const key = paramKey(p.index, p.subindex)
    const raw = editValues[key] ?? valueToInputString(p.value)
    const encoded = encodeSdoValue(sdoTypeForDataTypeName(p.dataTypeName), raw)
    if ('error' in encoded) {
      setRowMsg({ key, message: encoded.error, kind: 'error' })
      return
    }
    // Reuse the SDO encoder/decoder purely to validate the input and turn it into the typed JS
    // value the smart write expects (number / string / byte array) — the server coerces it to the
    // object's declared width.
    const sent = decodeSdoBytes(p.dataTypeName, encoded.bytes)
    setSettingKeys(prev => new Set(prev).add(key))
    setRowMsg(null)
    try {
      // Smart write: PDO-staged when the object is output-mapped + exchanging, SDO otherwise. The
      // device may still clamp, coerce, or ignore the value — so for readable objects read it back
      // (PDO-aware) rather than trusting what we sent. Write-only objects keep the sent value.
      const written = await api.writeParameter(slavePosition, p.index, p.subindex, { value: sent })
      const readBack = isReadable(p.access)
      const value = readBack
        ? (await api.readParameter(slavePosition, p.index, p.subindex)).data.value
        : written.data.value
      queryClient.setQueryData(paramsQueryKey, (prev: typeof paramsQuery.data) => {
        if (!prev) return prev
        const next = prev.data.map(x =>
          x.index === p.index && x.subindex === p.subindex
            ? { ...x, value, syncState: 'synced' as const }
            : x,
        )
        return { ...prev, data: next }
      })
      clearEdit(key)
      // If the read-back differs from what we sent, the device silently rejected or
      // adjusted the write (out-of-range clamp, rounding, ignored). Flag it — the
      // displayed value is now the device's actual value, not what was typed.
      if (readBack && !valuesEqual(value, sent)) {
        setRowMsg({
          key,
          message: 'Write was sent but the device kept a different value (shown) — out of range or not accepted.',
          kind: 'note',
        })
      }
    } catch (err) {
      setRowMsg({ key, message: apiError(err), kind: 'error' })
    } finally {
      setSettingKeys(prev => {
        const next = new Set(prev)
        next.delete(key)
        return next
      })
    }
  }

  const params: DeviceParameter[] = paramsQuery.data?.data ?? []
  const listError = initMutation.error
    ? apiError(initMutation.error)
    : readAllMutation.error
      ? apiError(readAllMutation.error)
      : null
  const listBusy = initMutation.isPending || readAllMutation.isPending || paramsQuery.isFetching

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

  const dlIndexNum = parseHexOrDec(dlIndex)
  const dlSubindexNum = parseHexOrDec(dlSubindex)
  const dlIndexValid = dlIndexNum !== null && dlIndexNum >= 0 && dlIndexNum <= 0xffff
  const dlSubindexValid = dlSubindexNum !== null && dlSubindexNum >= 0 && dlSubindexNum <= 0xff

  const encoded = encodeSdoValue(dlType, dlValue)
  const encodedBytes = 'bytes' in encoded ? encoded.bytes : null
  // Only surface an encoding error once the user has typed something.
  const encodeError = 'error' in encoded && dlValue.trim() !== '' ? encoded.error : null
  const canDownload = dlIndexValid && dlSubindexValid && encodedBytes !== null && !downloading

  function clearDownloadStatus() {
    setDlOk(null)
    setDlError(null)
  }

  async function handleDownload() {
    if (!canDownload || !encodedBytes) return
    setDownloading(true)
    setDlOk(null)
    setDlError(null)
    try {
      await api.sdoDownload(slavePosition, dlIndexNum!, dlSubindexNum!, { data: encodedBytes })
      setDlOk(encodedBytes)
    } catch (err) {
      setDlError(apiError(err))
    } finally {
      setDownloading(false)
    }
  }

  return (
    <div>
      <DevicePageHeader
        slavePosition={slavePosition}
        title="Parameters"
        description={
          <>
            Read and write the device's CoE object dictionary — the parameters addressed by index
            and subindex (e.g. 0x6064 position actual value). Reading and setting a value in the
            list below is PDO-aware: it uses the live process image (or stages an output, for an
            RxPDO like a target) while the device is exchanging, and falls back to SDO over the
            mailbox otherwise. The manual Upload/Download tool below is always raw SDO and works
            from PRE-OP up.
          </>
        }
      />
      <div className="p-4 sm:p-8 space-y-6">

        <div className="grid grid-cols-1 lg:grid-cols-2 gap-8 items-start">

        <section>
          <p className="eyebrow mb-5">Read SDO</p>
          <div className="border border-grey-200 p-5 space-y-4">

            <p className="text-xs text-grey-500">
              Raw, byte-level CoE SDO read (<span className="font-mono">readSdo</span>) of any
              object by index/subindex — no PDO awareness, always over the mailbox. To read a
              parameter by value (PDO-aware), use the <span className="font-mono">↻</span> in the
              parameter list below instead.
            </p>

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
              {uploading ? 'Reading…' : 'Read SDO'}
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
                {interpretSdoBytes(result).map(({ label, value }) => (
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
          <p className="eyebrow mb-5">Write SDO</p>
          <div className="border border-grey-200 p-5 space-y-4">

            <p className="text-xs text-grey-500">
              Raw, byte-level CoE SDO write (<span className="font-mono">writeSdo</span>) straight
              to the device's object dictionary — no PDO awareness, always over the mailbox. The
              cached value in the parameter list below is <strong>not</strong> updated — read it
              back with the row's <span className="font-mono">↻</span> to confirm. To set a
              parameter by value (PDO-aware), use the row's Set button instead.
            </p>

            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className={labelCls}>Index (hex or dec)</label>
                <input
                  type="text"
                  value={dlIndex}
                  onChange={e => { setDlIndex(e.target.value); clearDownloadStatus() }}
                  placeholder="e.g. 0x6060 or 24672"
                  className={inputCls}
                />
                <p className="text-xs text-grey-500 mt-1 font-mono">
                  {dlIndexValid ? toHex(dlIndexNum!, 4) : '—'}
                </p>
              </div>
              <div>
                <label className={labelCls}>Subindex (hex or dec)</label>
                <input
                  type="text"
                  value={dlSubindex}
                  onChange={e => { setDlSubindex(e.target.value); clearDownloadStatus() }}
                  placeholder="e.g. 0x00 or 0"
                  className={inputCls}
                />
                <p className="text-xs text-grey-500 mt-1 font-mono">
                  {dlSubindexValid ? toHex(dlSubindexNum!, 2) : '—'}
                </p>
              </div>
            </div>

            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className={labelCls}>Type</label>
                <select
                  value={dlType}
                  onChange={e => { setDlType(e.target.value as SdoType); clearDownloadStatus() }}
                  className={`${inputCls} cursor-pointer`}
                >
                  {SDO_TYPES.map(t => (
                    <option key={t} value={t}>{t}</option>
                  ))}
                </select>
              </div>
              <div>
                <label className={labelCls}>Value</label>
                <HexDecInput
                  value={dlValue}
                  onChange={v => { setDlValue(v); clearDownloadStatus() }}
                  canHex={isIntegerSdoType(dlType)}
                  placeholder={SDO_TYPE_HINT[dlType]}
                  wrapperClassName="flex w-full"
                  inputClassName={inputCls}
                />
                <p className="text-xs mt-1 font-mono">
                  {encodeError ? (
                    <span className="text-status-bad">{encodeError}</span>
                  ) : (
                    <span className="text-grey-500">
                      {encodedBytes
                        ? `${encodedBytes.length} B · ${encodedBytes.map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ')}`
                        : '—'}
                    </span>
                  )}
                </p>
              </div>
            </div>

            <button onClick={handleDownload} disabled={!canDownload} className={btnCls}>
              {downloading ? 'Writing…' : 'Write SDO'}
            </button>

            {dlError && (
              <p className="text-xs text-status-bad font-mono">{dlError}</p>
            )}

            {dlOk && (
              <div className="border border-grey-200 p-3 space-y-1 bg-grey-50">
                <p className="text-xs text-status-good uppercase tracking-wide mb-2">Wrote {dlOk.length} byte{dlOk.length === 1 ? '' : 's'}</p>
                <p className="text-xs font-mono">
                  <span className="text-grey-600">Hex:&nbsp;</span>
                  {dlOk.map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ')}
                </p>
                <p className="text-xs font-mono">
                  <span className="text-grey-600">Dec:&nbsp;</span>
                  [{dlOk.join(', ')}]
                </p>
              </div>
            )}

          </div>
        </section>

        </div>

        <section>
          <p className="eyebrow mb-5">Parameter list</p>
          <div className="border border-grey-200 p-5 space-y-4">
            <p className="text-xs text-grey-500">
              Reading (<span className="font-mono">↻</span>) and setting a value here is
              <strong> PDO-aware</strong> (<span className="font-mono">readParameter</span> /{' '}
              <span className="font-mono">writeParameter</span>): while the device is exchanging it
              uses the live process image — or stages an output, for an RxPDO like a target — and
              falls back to SDO over the mailbox otherwise. Use the Read/Write SDO tools above for
              raw byte-level access by index.
            </p>
            <div className="flex flex-wrap items-center gap-x-3 gap-y-2">
              {params.length > 0 && (
                <div className="flex items-center gap-3 flex-1 min-w-[12rem]">
                  <div className="relative max-w-sm w-full">
                    <input
                      type="text"
                      value={filter}
                      onChange={e => setFilter(e.target.value)}
                      placeholder="Filter by name, index, or type…"
                      className={`${inputCls} pr-8`}
                    />
                    {filter && (
                      <button
                        type="button"
                        onClick={() => setFilter('')}
                        className="absolute right-2 top-1/2 -translate-y-1/2 text-grey-400 hover:text-syn-red cursor-pointer leading-none text-lg"
                        title="Clear filter"
                        aria-label="Clear filter"
                      >
                        ×
                      </button>
                    )}
                  </div>
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
                  <span>Read values during re-initialize (SDO upload per entry — slower)</span>
                </label>
                <button
                  className={btnCls}
                  disabled={listBusy}
                  onClick={() => initMutation.mutate()}
                  title="Clear the parameter list and re-enumerate the device's object dictionary over SDO Info (rebuilds names/types/access). With the checkbox on, also reads every value during the rebuild. Can take several seconds."
                >
                  {initMutation.isPending ? 'Re-initialising…' : params.length === 0 ? 'Initialize' : 'Re-initialize'}
                </button>
                <button
                  className={btnGhostCls}
                  disabled={listBusy || params.length === 0}
                  onClick={() => readAllMutation.mutate()}
                  title="Re-read the value of every parameter without rebuilding the list (PDO-aware: live process image when exchanging, SDO otherwise). Write-only objects are skipped."
                >
                  {readAllMutation.isPending ? 'Reading…' : 'Read all values'}
                </button>
                <button
                  className={btnGhostCls}
                  disabled={listBusy || params.length === 0}
                  onClick={handleReload}
                  title="Re-fetch the cached parameter list from the server (no bus traffic). Use to pick up values changed elsewhere."
                >
                  {paramsQuery.isFetching ? 'Reloading…' : 'Reload list'}
                </button>
                {(initElapsedMs ?? readAllElapsedMs ?? reloadElapsedMs) !== null && (
                  <span className="text-xs text-grey-500 font-mono whitespace-nowrap">
                    took {formatElapsed((initElapsedMs ?? readAllElapsedMs ?? reloadElapsedMs)!)}
                  </span>
                )}
              </div>
            </div>

            {listError && (
              <p className="text-xs text-status-bad font-mono">{listError}</p>
            )}

            {params.length > 0 && (
              <p className="text-xs text-grey-500">
                <strong>Re-initialize</strong> rebuilds the list from the object dictionary (slow,
                SDO Info); <strong>Read all values</strong> refreshes every value in place without
                rebuilding (PDO-aware — live process image when exchanging, SDO otherwise);{' '}
                <strong>Reload list</strong> re-fetches the cached list from the server (no bus
                traffic). A row's <span className="font-mono">↻</span> reads one value live; edit a
                writable row and press <strong>Set</strong> to write it back (staged as a
                process-data output when exchanging, SDO otherwise) — read-only objects are locked.
              </p>
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
                        const setting = settingKeys.has(key)
                        const busy = refreshing || setting
                        const writable = isWritable(p.access)
                        const editValue = editValues[key] ?? valueToInputString(p.value)
                        const cellMsg = rowMsg?.key === key ? rowMsg : null
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
                              <div className="flex items-stretch gap-2">
                                <HexDecInput
                                  value={editValue}
                                  onChange={v => setEditValues(prev => ({ ...prev, [key]: v }))}
                                  canHex={isIntegerSdoType(sdoTypeForDataTypeName(p.dataTypeName))}
                                  disabled={!writable || busy}
                                  toggleDisabled={busy}
                                  hexPadDigits={Math.max(2, Math.ceil(p.bitLength / 4))}
                                  wrapperClassName="flex w-60"
                                  inputClassName="border border-grey-300 px-2 py-1 text-xs font-mono bg-white disabled:bg-grey-50 disabled:text-grey-500 disabled:cursor-not-allowed"
                                  title={writable ? 'Edit, then Set to write this parameter (PDO-aware: staged to the process image when exchanging, SDO otherwise). Toggle hex/dec to view or enter the value in either base.' : 'Read-only object'}
                                />
                                <button
                                  type="button"
                                  onClick={() => handleSetValue(p)}
                                  disabled={!writable || busy}
                                  className="inline-flex items-center justify-center border border-grey-300 px-3 text-xs text-syn-red hover:text-ocean hover:border-grey-400 disabled:opacity-40 disabled:cursor-not-allowed cursor-pointer font-display uppercase tracking-wide"
                                  title={writable ? 'Write this parameter (PDO-aware: staged to the process image when exchanging, SDO otherwise)' : 'Read-only object'}
                                >
                                  {setting ? '…' : 'Set'}
                                </button>
                                <button
                                  type="button"
                                  onClick={() => handleRefreshValue(p)}
                                  disabled={listBusy}
                                  className="inline-flex items-center justify-center border border-grey-300 px-2 text-grey-400 hover:text-syn-red hover:border-grey-400 disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer leading-none"
                                  title="Refresh this parameter (PDO-aware: live process image when exchanging, SDO otherwise)"
                                  aria-label="Refresh value"
                                >
                                  {refreshing ? '…' : '↻'}
                                </button>
                              </div>
                              {cellMsg && (
                                <p
                                  className={`mt-1 whitespace-normal ${
                                    cellMsg.kind === 'error' ? 'text-status-bad' : 'text-status-warn'
                                  }`}
                                >
                                  {cellMsg.message}
                                </p>
                              )}
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
