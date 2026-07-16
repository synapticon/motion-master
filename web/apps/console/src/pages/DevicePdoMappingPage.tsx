import { useEffect, useMemo, useState } from 'react'
import { useParams } from 'react-router'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { ArrowDown, ArrowUp, Plus, Trash2 } from 'lucide-react'
import {
  formatHex,
  type DeviceParameter,
  type PdoMapping,
  type PdoMappingRequest,
  type PdoMappingRequestObject,
} from '@synapticon/motion-master-client'
import Callout from '../components/Callout'
import DevicePageHeader from '../components/DevicePageHeader'
import ParameterPicker from '../components/ParameterPicker'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline } from '../utils/styles'

const AL_PRE_OP = 2

// ETG.1000.6 object-access (ObjAccess) mapping bits: an object is PDO-mappable in a direction only
// when the corresponding bit is set. Used to constrain the parameter picker per direction.
const RXPDO_MAPPABLE = 0x40 // master→slave (outputs / 0x1C12)
const TXPDO_MAPPABLE = 0x80 // slave→master (inputs / 0x1C13)

const btnPrimary =
  'bg-syn-red text-white px-4 py-1.5 text-xs hover:bg-ocean disabled:opacity-50 ' +
  'disabled:cursor-not-allowed cursor-pointer transition-colors'
const inputCls = 'border border-grey-300 px-2 py-1 text-xs font-mono bg-white'
const iconBtn =
  'p-1 text-grey-400 hover:text-grey-800 disabled:opacity-30 disabled:cursor-not-allowed ' +
  'cursor-pointer transition-colors'

// Draft fields are kept as strings so intermediate typing (e.g. "0x6") is allowed; they are parsed
// and range-checked only when building the request.
interface DraftEntry {
  index: string
  subindex: string
  bitLength: string
}
interface DraftObject {
  pdoIndex: string
  entries: DraftEntry[]
}
interface Draft {
  outputs: DraftObject[]
  inputs: DraftObject[]
}

type Direction = keyof Draft

// Parses a hex ("0x...") or decimal string to a number, or NaN if it is neither.
function parseNum(s: string): number {
  const t = s.trim()
  if (/^0x[0-9a-f]+$/i.test(t)) {
    return parseInt(t.slice(2), 16)
  }
  if (/^[0-9]+$/.test(t)) {
    return parseInt(t, 10)
  }
  return NaN
}

// A valid integer parse, or null (so it can feed the picker's selected index/subindex).
function parsedOrNull(s: string): number | null {
  const n = parseNum(s)
  return Number.isInteger(n) ? n : null
}

// Converts a device read-back into an editable draft (indices as hex, subindex/bitLength as decimal;
// the derived bitOffset is dropped — it is recomputed live and re-derived by the device on write).
function fromResponse(m: PdoMapping): Draft {
  const conv = (objs: PdoMapping['outputs']): DraftObject[] =>
    objs.map(o => ({
      pdoIndex: formatHex(o.pdoIndex),
      entries: o.entries.map(e => ({
        index: formatHex(e.index),
        subindex: String(e.subindex),
        bitLength: String(e.bitLength),
      })),
    }))
  return { outputs: conv(m.outputs), inputs: conv(m.inputs) }
}

// Validates + converts one direction to request objects. Throws a human-readable string on the first
// out-of-range field (index/pdoIndex 16-bit, subindex/bitLength 8-bit).
function toRequestObjects(objs: DraftObject[], dir: string): PdoMappingRequestObject[] {
  return objs.map((o, oi) => {
    const pdoIndex = parseNum(o.pdoIndex)
    if (!Number.isInteger(pdoIndex) || pdoIndex < 0 || pdoIndex > 0xffff) {
      throw `${dir} object ${oi + 1}: pdoIndex must be 0…0xFFFF`
    }
    const where = `${dir} ${formatHex(pdoIndex)}`
    const entries = o.entries.map((e, ei) => {
      const index = parseNum(e.index)
      const subindex = parseNum(e.subindex)
      const bitLength = parseNum(e.bitLength)
      if (!Number.isInteger(index) || index < 0 || index > 0xffff) {
        throw `${where} entry ${ei + 1}: index must be 0…0xFFFF`
      }
      if (!Number.isInteger(subindex) || subindex < 0 || subindex > 0xff) {
        throw `${where} entry ${ei + 1}: subindex must be 0…255`
      }
      if (!Number.isInteger(bitLength) || bitLength < 0 || bitLength > 0xff) {
        throw `${where} entry ${ei + 1}: bitLength must be 0…255`
      }
      return { index, subindex, bitLength }
    })
    return { pdoIndex, entries }
  })
}

// Sum of an object's mapped bit widths (invalid/blank entries count as 0), for the header badge.
function objectBits(obj: DraftObject): number {
  return obj.entries.reduce((sum, e) => {
    const n = parseNum(e.bitLength)
    return sum + (Number.isInteger(n) && n > 0 ? n : 0)
  }, 0)
}

// Immutable list helpers.
function replaceAt<T>(a: T[], i: number, v: T): T[] {
  return a.map((x, j) => (j === i ? v : x))
}
function removeAt<T>(a: T[], i: number): T[] {
  return a.filter((_, j) => j !== i)
}
function moveAt<T>(a: T[], i: number, delta: number): T[] {
  const j = i + delta
  if (j < 0 || j >= a.length) {
    return a
  }
  const c = a.slice()
  ;[c[i], c[j]] = [c[j], c[i]]
  return c
}

function apiError(err: unknown): string {
  if (err && typeof err === 'object' && 'error' in err) {
    const inner = (err as { error: unknown }).error
    if (inner && typeof inner === 'object' && 'error' in inner) {
      return String((inner as { error: unknown }).error)
    }
    if (typeof inner === 'string') {
      return inner
    }
  }
  return 'Unknown error'
}

interface EntryRowProps {
  entry: DraftEntry
  offset: number
  params: DeviceParameter[]
  mappableBit: number
  mappableHint: string
  first: boolean
  last: boolean
  onChange: (e: DraftEntry) => void
  onMove: (delta: number) => void
  onRemove: () => void
}

function EntryRow({
  entry,
  offset,
  params,
  mappableBit,
  mappableHint,
  first,
  last,
  onChange,
  onMove,
  onRemove,
}: EntryRowProps) {
  return (
    <div className="flex flex-wrap items-center gap-2 px-3 py-1.5 border-t border-grey-100">
      <ParameterPicker
        className="flex-1 min-w-[14rem]"
        params={params}
        selectedIndex={parsedOrNull(entry.index)}
        selectedSubindex={parsedOrNull(entry.subindex)}
        placeholder="Pick a mappable object…"
        isDisabled={p => (p.access & mappableBit) === 0}
        disabledHint={mappableHint}
        onSelect={p =>
          onChange({
            index: formatHex(p.index),
            subindex: String(p.subindex),
            bitLength: String(p.bitLength),
          })
        }
      />
      <input
        className={`${inputCls} w-24`}
        value={entry.index}
        onChange={e => onChange({ ...entry, index: e.target.value })}
        placeholder="0x6040"
        aria-label="Entry index"
      />
      <input
        className={`${inputCls} w-14`}
        value={entry.subindex}
        onChange={e => onChange({ ...entry, subindex: e.target.value })}
        placeholder="0"
        aria-label="Entry subindex"
      />
      <input
        className={`${inputCls} w-14`}
        value={entry.bitLength}
        onChange={e => onChange({ ...entry, bitLength: e.target.value })}
        placeholder="16"
        aria-label="Entry bit length"
      />
      <span className="w-14 font-mono text-xs text-grey-500 text-right">
        {Math.floor(offset / 8)}.{offset % 8}
      </span>
      <div className="flex items-center gap-0.5">
        <button className={iconBtn} disabled={first} onClick={() => onMove(-1)} title="Move entry up">
          <ArrowUp size={14} />
        </button>
        <button className={iconBtn} disabled={last} onClick={() => onMove(1)} title="Move entry down">
          <ArrowDown size={14} />
        </button>
        <button
          className={`${iconBtn} hover:text-status-bad`}
          onClick={onRemove}
          title="Remove entry"
        >
          <Trash2 size={14} />
        </button>
      </div>
    </div>
  )
}

interface DirectionEditorProps {
  label: string
  sm: string
  defaultPdoIndex: string
  objects: DraftObject[]
  params: DeviceParameter[]
  mappableBit: number
  mappableHint: string
  onChange: (objs: DraftObject[]) => void
}

// Editor for one direction (outputs / inputs): a list of mapping objects, each with an editable
// pdoIndex and an ordered list of entries. Running bit offsets and totals are computed live so the
// resulting process-image layout is visible while editing.
function DirectionEditor({
  label,
  sm,
  defaultPdoIndex,
  objects,
  params,
  mappableBit,
  mappableHint,
  onChange,
}: DirectionEditorProps) {
  let runningBits = 0
  const offsets = objects.map(o =>
    o.entries.map(e => {
      const at = runningBits
      const len = parseNum(e.bitLength)
      runningBits += Number.isInteger(len) && len > 0 ? len : 0
      return at
    }),
  )
  const totalBits = runningBits

  const setObject = (oi: number, obj: DraftObject) => onChange(replaceAt(objects, oi, obj))

  return (
    <section>
      <div className="flex items-baseline justify-between mb-3">
        <p className="eyebrow">
          {label} <span className="text-grey-400">· {sm}</span>
        </p>
        <p className="text-[11px] text-grey-500 font-mono">
          {objects.length} object{objects.length === 1 ? '' : 's'} · {totalBits} bit
          {totalBits === 1 ? '' : 's'} ({Math.ceil(totalBits / 8)} byte
          {Math.ceil(totalBits / 8) === 1 ? '' : 's'})
        </p>
      </div>

      {objects.length === 0 && (
        <p className="text-xs text-grey-500 mb-3">
          No mapping objects — writing this will clear the sync manager's assignment.
        </p>
      )}

      <div className="space-y-3">
        {objects.map((obj, oi) => (
          <div key={oi} className="border border-grey-200">
            <div className="flex items-center gap-2 bg-grey-50 border-b border-grey-200 px-3 py-2">
              <label className="text-[10px] uppercase tracking-wide text-grey-500 font-display">
                Object
              </label>
              <input
                className={`${inputCls} w-28`}
                value={obj.pdoIndex}
                onChange={e => setObject(oi, { ...obj, pdoIndex: e.target.value })}
                aria-label="Mapping object index"
              />
              <span className="text-[11px] text-grey-500 font-mono">
                {obj.entries.length} entr{obj.entries.length === 1 ? 'y' : 'ies'} ·{' '}
                {objectBits(obj)} bit{objectBits(obj) === 1 ? '' : 's'}
              </span>
              <div className="ml-auto flex items-center gap-0.5">
                <button
                  className={iconBtn}
                  disabled={oi === 0}
                  onClick={() => onChange(moveAt(objects, oi, -1))}
                  title="Move object up"
                >
                  <ArrowUp size={14} />
                </button>
                <button
                  className={iconBtn}
                  disabled={oi === objects.length - 1}
                  onClick={() => onChange(moveAt(objects, oi, 1))}
                  title="Move object down"
                >
                  <ArrowDown size={14} />
                </button>
                <button
                  className={`${iconBtn} hover:text-status-bad`}
                  onClick={() => onChange(removeAt(objects, oi))}
                  title="Remove object"
                >
                  <Trash2 size={14} />
                </button>
              </div>
            </div>

            {obj.entries.length > 0 && (
              <div className="flex flex-wrap items-center gap-2 px-3 pt-2 text-[10px] uppercase tracking-wide text-grey-500 font-display">
                <span className="flex-1 min-w-[14rem]">Parameter</span>
                <span className="w-24">Index</span>
                <span className="w-14">Sub</span>
                <span className="w-14">Bits</span>
                <span className="w-14 text-right">Byte.Bit</span>
                <span className="w-[70px]" />
              </div>
            )}

            {obj.entries.map((entry, ei) => (
              <EntryRow
                key={ei}
                entry={entry}
                offset={offsets[oi][ei]}
                params={params}
                mappableBit={mappableBit}
                mappableHint={mappableHint}
                first={ei === 0}
                last={ei === obj.entries.length - 1}
                onChange={v => setObject(oi, { ...obj, entries: replaceAt(obj.entries, ei, v) })}
                onMove={delta => setObject(oi, { ...obj, entries: moveAt(obj.entries, ei, delta) })}
                onRemove={() => setObject(oi, { ...obj, entries: removeAt(obj.entries, ei) })}
              />
            ))}

            <div className="border-t border-grey-100 px-3 py-2">
              <button
                className="text-xs text-syn-red hover:underline cursor-pointer inline-flex items-center gap-1"
                onClick={() =>
                  setObject(oi, {
                    ...obj,
                    entries: [...obj.entries, { index: '0x0000', subindex: '0', bitLength: '0' }],
                  })
                }
              >
                <Plus size={13} /> Add entry
              </button>
            </div>
          </div>
        ))}
      </div>

      <button
        className={`${btnOutline} mt-3 inline-flex items-center gap-1`}
        onClick={() => onChange([...objects, { pdoIndex: defaultPdoIndex, entries: [] }])}
      >
        <Plus size={13} /> Add mapping object
      </button>
    </section>
  )
}

export default function DevicePdoMappingPage() {
  const { deviceId } = useParams()
  const { api } = useConnection()
  const queryClient = useQueryClient()
  const slavePosition = Number(deviceId)

  const mappingKey = ['pdoMapping', slavePosition]
  const query = useQuery({
    queryKey: mappingKey,
    queryFn: () => api.getDevicePdoMapping(slavePosition),
  })
  const loaded = query.data?.data

  // The device's object dictionary drives the parameter picker (name search + PDO-mappability +
  // bit length). Shares the Parameters page's query cache (identical key), so it neither
  // double-fetches nor drifts. Empty until the OD has been enumerated (device reached PRE-OP).
  const paramsQuery = useQuery({
    queryKey: ['deviceParameters', slavePosition],
    queryFn: () => api.getDeviceParameters(slavePosition),
    staleTime: Infinity,
  })
  const params = paramsQuery.data?.data ?? []

  const statesQuery = useQuery({
    queryKey: ['deviceStates'],
    queryFn: () => api.getDeviceStates(),
    refetchInterval: 2000,
  })
  const state = statesQuery.data?.data.find(s => s.slavePosition === slavePosition)
  const inPreOp = state?.alState === AL_PRE_OP

  const [draft, setDraft] = useState<Draft | null>(null)
  const [writeError, setWriteError] = useState<string | null>(null)
  const [saved, setSaved] = useState(false)

  // Seed (and reseed) the draft from the device whenever a fresh read-back arrives — the initial
  // load, a manual reload, or the read-back echoed by a successful write.
  useEffect(() => {
    if (loaded) {
      setDraft(fromResponse(loaded))
    }
  }, [loaded])

  const baseline = useMemo(() => (loaded ? fromResponse(loaded) : null), [loaded])
  const dirty =
    draft != null && baseline != null && JSON.stringify(draft) !== JSON.stringify(baseline)

  const built = useMemo(() => {
    if (!draft) {
      return { body: undefined as PdoMappingRequest | undefined, error: undefined as string | undefined }
    }
    try {
      return {
        body: {
          outputs: toRequestObjects(draft.outputs, 'output'),
          inputs: toRequestObjects(draft.inputs, 'input'),
        } as PdoMappingRequest,
        error: undefined as string | undefined,
      }
    } catch (e) {
      return { body: undefined as PdoMappingRequest | undefined, error: String(e) }
    }
  }, [draft])

  const writeMutation = useMutation({
    mutationFn: () => api.writeDevicePdoMapping(slavePosition, built.body as PdoMappingRequest),
    onSuccess: res => {
      queryClient.setQueryData(mappingKey, res)
      setWriteError(null)
      setSaved(true)
    },
    onError: e => {
      setWriteError(apiError(e))
      setSaved(false)
    },
  })

  const setDirection = (dir: Direction, objs: DraftObject[]) => {
    setSaved(false)
    setDraft(d => (d ? { ...d, [dir]: objs } : d))
  }

  return (
    <div>
      <DevicePageHeader
        slavePosition={slavePosition}
        title="PDO Mapping"
        description="Edit which objects the device exchanges cyclically, and where they sit in the process image."
      />
      <div className="p-4 sm:px-8 sm:py-7 space-y-6">
        <div className="space-y-4">
          <Callout variant="info">
            Rewrites the device's sync-manager PDO assignment (0x1C12 / 0x1C13) and mapping objects
            (0x16xx / 0x1Axx) over CoE. <strong>The device must be in PRE-OP</strong> — the mapping
            is writable only there. Take it to PRE-OP on the Control page, write here, then bring it
            back to SAFE-OP/OP, which re-maps the whole-bus process image. The write is verified
            against a read-back; it does not change the AL state itself.
          </Callout>

          {state && !inPreOp && (
            <Callout variant="warning">
              The device is not in PRE-OP, so a write will be rejected. Take it to PRE-OP first
              (Control page) to edit its mapping.
            </Callout>
          )}

          {query.isError && (
            <Callout variant="error">
              Could not read the current mapping: {apiError(query.error)}. The device's mailbox must
              be active (PRE-OP or higher).
            </Callout>
          )}

          {paramsQuery.data && params.length === 0 && (
            <Callout variant="info">
              The object dictionary has not been enumerated for this device, so the parameter picker
              is empty — you can still enter index, subindex, and bit length by hand. Open the
              Parameters page to read the dictionary.
            </Callout>
          )}
        </div>

        <div className="flex items-center gap-3">
          <button className={btnOutline} onClick={() => query.refetch()} disabled={query.isFetching}>
            {query.isFetching ? 'Loading…' : 'Reload from device'}
          </button>
          <button
            className={btnPrimary}
            onClick={() => writeMutation.mutate()}
            disabled={!dirty || !built.body || writeMutation.isPending}
          >
            {writeMutation.isPending ? 'Writing…' : 'Write mapping'}
          </button>
          {dirty && <span className="text-xs text-status-warn">Unsaved changes</span>}
          {!dirty && saved && <span className="text-xs text-status-good">Mapping written ✓</span>}
        </div>

        {built.error && <Callout variant="error">{built.error}</Callout>}
        {writeError && <Callout variant="error">Write failed: {writeError}</Callout>}

        {draft && (
          <div className="grid grid-cols-1 lg:grid-cols-2 gap-8 items-start">
            <DirectionEditor
              label="Outputs · RxPDO"
              sm="0x1C12"
              defaultPdoIndex="0x1600"
              objects={draft.outputs}
              params={params}
              mappableBit={RXPDO_MAPPABLE}
              mappableHint="not RxPDO-mappable"
              onChange={objs => setDirection('outputs', objs)}
            />
            <DirectionEditor
              label="Inputs · TxPDO"
              sm="0x1C13"
              defaultPdoIndex="0x1A00"
              objects={draft.inputs}
              params={params}
              mappableBit={TXPDO_MAPPABLE}
              mappableHint="not TxPDO-mappable"
              onChange={objs => setDirection('inputs', objs)}
            />
          </div>
        )}
      </div>
    </div>
  )
}
