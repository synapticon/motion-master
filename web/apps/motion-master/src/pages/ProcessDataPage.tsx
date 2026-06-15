import { useEffect, useMemo, useRef, useState } from 'react'
import { useMutation, useQueries, useQuery } from '@tanstack/react-query'
import { AlertTriangle, Send } from 'lucide-react'
import {
  type DeviceParameter,
  type ProcessImageObject,
  formatHex,
} from '@synapticon/motion-master-client'
import PageHeader from '../components/PageHeader'
import SlavePositionBadge from '../components/SlavePositionBadge'
import { useConnection } from '../contexts/ConnectionContext'
import {
  MonitoringSocketProvider,
  type SampleRows,
  useMonitoringSocket,
} from '../contexts/MonitoringSocketContext'

const inputCls = 'border border-grey-300 px-2 py-1 text-xs w-32 font-mono bg-white'
const btnCls =
  'bg-syn-red text-white px-4 py-2 text-xs hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'
const btnGhostCls =
  'border border-grey-300 text-grey-700 px-4 py-2 text-xs hover:bg-grey-50 disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

// The page's reserved monitoring topic. It mirrors the whole process image (both directions), is
// recreated whenever the layout changes, and is deleted when the page unmounts. ('pdos' is reserved
// by the server for a future built-in stream, so we use our own distinct name.)
const TOPIC = 'process-data'
// Flush cadence for the live stream (ms). 100 ms ≈ 10 Hz — plenty for a values panel and far cheaper
// than chart-grade rates; the stream stays lossless regardless (every cycle is delivered per batch).
const FLUSH_MS = 100

function apiError(err: unknown): string {
  if (err && typeof err === 'object') {
    if ('error' in err) {
      const inner = (err as { error: unknown }).error
      if (inner && typeof inner === 'object' && 'error' in inner) {
        return String((inner as { error: unknown }).error)
      }
      if (typeof inner === 'string') return inner
    }
    if ('status' in err && typeof (err as { status: unknown }).status === 'number') {
      return `HTTP ${(err as { status: number }).status}`
    }
  }
  return 'Unknown error'
}

function parseHexOrDec(s: string): number | null {
  const t = s.trim()
  if (/^[-+]?0[xX][0-9a-fA-F]+$/.test(t)) return parseInt(t.replace(/0[xX]/, ''), 16)
  if (/^[-+]?[0-9]+$/.test(t)) return parseInt(t, 10)
  return null
}

function objKey(pos: number, index: number, subindex: number): string {
  return `${pos}:${index}:${subindex}`
}

function objKeyOf(o: ProcessImageObject): string {
  return objKey(o.slavePosition, o.index, o.subindex)
}

// Classify how a value should be parsed/edited from the object's declared data type. Most PDO
// objects are integers; REAL32/REAL64 are floats; the *_STRING types are text. The backend coerces
// the JSON value to the declared type either way — this only picks the right input parsing.
type ValueKind = 'int' | 'float' | 'string'
function valueKind(dataTypeName: string | undefined): ValueKind {
  if (!dataTypeName) return 'int'
  if (dataTypeName.startsWith('REAL')) return 'float'
  if (dataTypeName.includes('STRING')) return 'string'
  return 'int'
}

// Parse one edited field into the JSON value to send, per its data type. Returns an error string
// when the text can't be parsed.
function parseValue(
  text: string,
  kind: ValueKind,
): { ok: true; value: number | string } | { ok: false; error: string } {
  if (kind === 'string') return { ok: true, value: text }
  const t = text.trim()
  if (t === '') return { ok: false, error: 'empty' }
  if (kind === 'float') {
    const n = Number(t)
    return Number.isFinite(n) ? { ok: true, value: n } : { ok: false, error: 'not a number' }
  }
  const n = parseHexOrDec(t)
  return n === null
    ? { ok: false, error: 'not an integer (use decimal or 0x…)' }
    : { ok: true, value: n }
}

// Format a live value for display. null = the device was not exchanging at sample time (or no data
// yet); numbers print as decimal.
function formatLive(v: number | null): string {
  if (v === null || v === undefined) return '—'
  return String(v)
}

export default function ProcessDataPage() {
  const { api, hasScanned, host, wsPort } = useConnection()
  const wsUrl = `wss://${host}:${wsPort}`

  const imageQuery = useQuery({
    queryKey: ['processImage'],
    queryFn: () => api.getProcessImage(),
    enabled: hasScanned,
    refetchInterval: 2000,
  })
  const img = imageQuery.data?.data

  const devicesQuery = useQuery({
    queryKey: ['devices'],
    queryFn: () => api.getDevices(),
    enabled: hasScanned,
    staleTime: Infinity,
  })
  const devices = devicesQuery.data?.data ?? []
  const deviceName = (pos: number) => devices.find((d) => d.slavePosition === pos)?.name ?? ''

  // Object dictionaries per device, so we can classify each object's data type (int/float/string)
  // for the right editor and parsing. Empty for devices whose OD has not been enumerated.
  const paramQueries = useQueries({
    queries: devices.map((d) => ({
      queryKey: ['deviceParameters', d.slavePosition],
      queryFn: () => api.getDeviceParameters(d.slavePosition),
      enabled: hasScanned,
      staleTime: Infinity,
    })),
  })
  const paramByKey = useMemo(() => {
    const map = new Map<string, DeviceParameter>()
    devices.forEach((d, i) => {
      for (const p of paramQueries[i]?.data?.data ?? []) {
        map.set(objKey(d.slavePosition, p.index, p.subindex), p)
      }
    })
    return map
  }, [devices, paramQueries])

  if (!hasScanned) {
    return (
      <div>
        <PageHeader eyebrow="Data" title="Process Data" />
        <p className="px-8 py-6 text-sm text-grey-600">
          Connect and scan the bus, then bring devices to SAFE-OP or OP to view and write process
          data.
        </p>
      </div>
    )
  }

  const idle = !img || (!img.configured && img.generations === 0)

  return (
    <div>
      <PageHeader
        eyebrow="Data"
        title="Process Data"
        description="Live two-column view of the whole process image: read inputs (TxPDO) streaming from the devices, and write outputs (RxPDO). Set output values and send them all at once — each is staged into the output image and sent on the next real-time cycle, then re-sent every cycle."
      />

      <div className="px-4 sm:px-8 py-6 space-y-6">
        <div className="flex items-start gap-2 border border-syn-red/40 bg-syn-red/5 px-4 py-3 text-xs text-grey-700">
          <AlertTriangle className="h-4 w-4 text-syn-red shrink-0 mt-0.5" />
          <p>
            Writing an output stages it straight into the drive's process data and re-sends it every
            cycle. Manually setting <span className="font-mono">controlword</span>, modes of
            operation, or a motion target can cause the drive to move. Make sure the machine is safe
            before sending.
          </p>
        </div>

        {imageQuery.isError && (
          <p className="text-xs text-status-bad font-mono">Failed to load the process image.</p>
        )}

        {idle && (
          <p className="text-xs text-grey-600">
            No process image is published. Bring a device to SAFE-OP or OP to map process data.
          </p>
        )}

        {img && !idle && (
          <>
            {!img.configured && (
              <div className="border border-status-warn/40 bg-status-warn/5 px-4 py-3 text-xs text-grey-700">
                Not currently exchanging — all devices have left SAFE-OP/OP. Showing the most recent
                published layout (generation {img.generations}); live values are unavailable and
                writes are not cyclically sent until a device re-enters SAFE-OP/OP.
              </div>
            )}
            <div className="flex flex-wrap gap-x-6 gap-y-1 text-xs text-grey-600">
              <span>
                Output bytes <span className="font-mono text-grey-800">{img.outputBytes}</span>
              </span>
              <span>
                Input bytes <span className="font-mono text-grey-800">{img.inputBytes}</span>
              </span>
              <span>
                Working counter{' '}
                <span
                  className={`font-mono ${
                    !img.configured
                      ? 'text-grey-400'
                      : img.healthy
                        ? 'text-status-good'
                        : 'text-status-bad'
                  }`}
                >
                  {img.configured ? `${img.lastWkc} / ${img.expectedWkc}` : 'idle'}
                </span>
              </span>
            </div>

            <MonitoringSocketProvider key={wsUrl} url={wsUrl}>
              <ProcessDataPanels
                outputs={img.outputs}
                inputs={img.inputs}
                configured={img.configured}
                deviceName={deviceName}
                paramByKey={paramByKey}
              />
            </MonitoringSocketProvider>
          </>
        )}
      </div>
    </div>
  )
}

// --- Live panels (inside the WebSocket provider) -----------------------------

interface StageOutcome {
  staged: boolean
  error: string
}

function ProcessDataPanels({
  outputs,
  inputs,
  configured,
  deviceName,
  paramByKey,
}: {
  outputs: ProcessImageObject[]
  inputs: ProcessImageObject[]
  configured: boolean
  deviceName: (pos: number) => string
  paramByKey: Map<string, DeviceParameter>
}) {
  const { api } = useConnection()
  const { subscribe } = useMonitoringSocket()

  // The monitoring is created over [outputs…, inputs…], so live[k] aligns with that order: outputs
  // occupy [0, outputs.length), inputs the rest. A signature of the object list drives recreation
  // when the layout changes (a re-map).
  const sig = useMemo(() => [...outputs, ...inputs].map(objKeyOf).join('|'), [outputs, inputs])
  const parameters = useMemo(
    () => [...outputs, ...inputs].map((o) => [o.slavePosition, o.index, o.subindex]),
    [sig],
  )

  // (Re)create the reserved monitoring for the current layout. A token guards against an older
  // delete→create sequence finishing after a newer one (so the latest layout always wins); the
  // unmount effect below removes it once.
  const tokenRef = useRef<object>({})
  useEffect(() => {
    if (parameters.length === 0) return
    const token = {}
    tokenRef.current = token
    void (async () => {
      try {
        await api.deleteMonitoring(TOPIC)
      } catch {
        // Not present yet — fine.
      }
      if (tokenRef.current !== token) return
      try {
        await api.createMonitoring({
          topic: TOPIC,
          name: 'Process Data (page)',
          interval: FLUSH_MS,
          parameters,
        })
      } catch {
        // A racing tab may already hold it; we still subscribe and read whatever it publishes.
      }
    })()
  }, [api, sig, parameters])

  useEffect(
    () => () => {
      api.deleteMonitoring(TOPIC).catch(() => {})
    },
    [api],
  )

  // Latest value per object, positionally aligned with `parameters`. Reset on layout change.
  const [live, setLive] = useState<(number | null)[]>(() => parameters.map(() => null))
  useEffect(() => {
    setLive(parameters.map(() => null))
    const onBatch = (rows: SampleRows) => {
      if (rows.length === 0) return
      const last = rows[rows.length - 1]
      setLive(
        parameters.map((_, k) => (typeof last[k + 1] === 'number' ? (last[k + 1] as number) : null)),
      )
    }
    return subscribe(TOPIC, onBatch)
  }, [subscribe, sig, parameters])

  // Edited output values (raw text), keyed by object. Only edited fields are sent.
  const [edits, setEdits] = useState<Map<string, string>>(new Map())
  const [outcomes, setOutcomes] = useState<Map<string, StageOutcome>>(new Map())
  const [formError, setFormError] = useState<string | null>(null)

  function setEdit(key: string, text: string) {
    setEdits((m) => {
      const next = new Map(m)
      next.set(key, text)
      return next
    })
  }

  const sendMutation = useMutation({
    mutationFn: (rows: (number | string)[][]) => api.stageProcessDataOutputs(rows),
  })

  function sendAll() {
    setFormError(null)
    const rows: (number | string)[][] = []
    for (const o of outputs) {
      const key = objKeyOf(o)
      const text = edits.get(key)
      if (text === undefined || text === '') continue
      const kind = valueKind(paramByKey.get(key)?.dataTypeName)
      const parsed = parseValue(text, kind)
      if (!parsed.ok) {
        setFormError(`${formatHex(o.index)}:${formatHex(o.subindex, 2, false)} — ${parsed.error}`)
        return
      }
      rows.push([o.slavePosition, o.index, o.subindex, parsed.value])
    }
    if (rows.length === 0) {
      setFormError('No edited outputs to send. Type a value into an output field first.')
      return
    }
    sendMutation.mutate(rows, {
      onSuccess: (res) => {
        const results = res.data?.results ?? []
        const nextOutcomes = new Map(outcomes)
        const stagedKeys = new Set<string>()
        for (const r of results) {
          const key = objKey(r.slavePosition, r.index, r.subindex)
          nextOutcomes.set(key, { staged: r.staged, error: r.error })
          if (r.staged) stagedKeys.add(key)
        }
        setOutcomes(nextOutcomes)
        // Clear edits that landed; keep the ones that errored so the user can fix them.
        setEdits((m) => {
          const next = new Map(m)
          for (const key of stagedKeys) next.delete(key)
          return next
        })
      },
      onError: (err) => setFormError(apiError(err)),
    })
  }

  const dirtyCount = outputs.reduce((n, o) => {
    const t = edits.get(objKeyOf(o))
    return t !== undefined && t !== '' ? n + 1 : n
  }, 0)

  return (
    <div className="grid grid-cols-1 gap-6 xl:grid-cols-2">
      {/* Outputs (editable) */}
      <section className="space-y-3">
        <div className="flex items-center justify-between">
          <p className="eyebrow">
            Outputs · RxPDO <span className="text-grey-400">({outputs.length})</span>
          </p>
          <div className="flex items-center gap-2">
            <button
              type="button"
              className={btnGhostCls}
              onClick={() => {
                setEdits(new Map())
                setFormError(null)
              }}
              disabled={dirtyCount === 0 || sendMutation.isPending}
            >
              Clear
            </button>
            <button
              type="button"
              className={`${btnCls} inline-flex items-center gap-1.5`}
              onClick={sendAll}
              disabled={dirtyCount === 0 || sendMutation.isPending || !configured}
            >
              <Send className="h-4 w-4" />
              {sendMutation.isPending
                ? 'Sending…'
                : `Send all${dirtyCount ? ` (${dirtyCount})` : ''}`}
            </button>
          </div>
        </div>

        {formError && <p className="text-xs text-syn-red">{formError}</p>}

        {outputs.length === 0 ? (
          <p className="text-xs text-grey-500">No output objects mapped.</p>
        ) : (
          <div className="border border-grey-200 overflow-x-auto">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50 text-left text-grey-600">
                  <th className="px-3 py-2 font-display uppercase tracking-wide font-medium">Slave</th>
                  <th className="px-3 py-2 font-display uppercase tracking-wide font-medium">Object</th>
                  <th className="px-3 py-2 font-display uppercase tracking-wide font-medium">Name</th>
                  <th className="px-3 py-2 font-display uppercase tracking-wide font-medium">Value</th>
                  <th className="px-3 py-2 font-display uppercase tracking-wide font-medium">Sent</th>
                </tr>
              </thead>
              <tbody>
                {outputs.map((o, i) => {
                  const key = objKeyOf(o)
                  const sent = live[i] ?? null
                  const outcome = outcomes.get(key)
                  return (
                    <tr key={key} className="border-b border-grey-100 last:border-0 align-top">
                      <td className="px-3 py-2" title={deviceName(o.slavePosition)}>
                        <SlavePositionBadge position={o.slavePosition} />
                      </td>
                      <td className="px-3 py-2 font-mono whitespace-nowrap">
                        {formatHex(o.index)}:{formatHex(o.subindex, 2, false)}
                      </td>
                      <td className="px-3 py-2 text-grey-700 truncate max-w-[12rem]" title={o.name}>
                        {o.name || <span className="text-grey-400">—</span>}
                      </td>
                      <td className="px-3 py-2">
                        <input
                          className={inputCls}
                          value={edits.get(key) ?? ''}
                          placeholder={formatLive(sent)}
                          onChange={(e) => setEdit(key, e.target.value)}
                        />
                        {outcome && (
                          <p
                            className={`mt-1 text-[10px] ${outcome.staged ? 'text-status-good' : 'text-syn-red'}`}
                            title={outcome.error}
                          >
                            {outcome.staged ? 'staged ✓' : outcome.error || 'not staged'}
                          </p>
                        )}
                      </td>
                      <td className="px-3 py-2 font-mono text-grey-600">{formatLive(sent)}</td>
                    </tr>
                  )
                })}
              </tbody>
            </table>
          </div>
        )}
      </section>

      {/* Inputs (live) */}
      <section className="space-y-3">
        <p className="eyebrow">
          Inputs · TxPDO <span className="text-grey-400">({inputs.length})</span>
        </p>
        {inputs.length === 0 ? (
          <p className="text-xs text-grey-500">No input objects mapped.</p>
        ) : (
          <div className="border border-grey-200 overflow-x-auto">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50 text-left text-grey-600">
                  <th className="px-3 py-2 font-display uppercase tracking-wide font-medium">Slave</th>
                  <th className="px-3 py-2 font-display uppercase tracking-wide font-medium">Object</th>
                  <th className="px-3 py-2 font-display uppercase tracking-wide font-medium">Name</th>
                  <th className="px-3 py-2 font-display uppercase tracking-wide font-medium">Value</th>
                </tr>
              </thead>
              <tbody>
                {inputs.map((o, j) => {
                  const value = live[outputs.length + j] ?? null
                  return (
                    <tr key={objKeyOf(o)} className="border-b border-grey-100 last:border-0">
                      <td className="px-3 py-2" title={deviceName(o.slavePosition)}>
                        <SlavePositionBadge position={o.slavePosition} />
                      </td>
                      <td className="px-3 py-2 font-mono whitespace-nowrap">
                        {formatHex(o.index)}:{formatHex(o.subindex, 2, false)}
                      </td>
                      <td className="px-3 py-2 text-grey-700 truncate max-w-[12rem]" title={o.name}>
                        {o.name || <span className="text-grey-400">—</span>}
                      </td>
                      <td className="px-3 py-2 font-mono text-grey-800">{formatLive(value)}</td>
                    </tr>
                  )
                })}
              </tbody>
            </table>
          </div>
        )}
      </section>
    </div>
  )
}
