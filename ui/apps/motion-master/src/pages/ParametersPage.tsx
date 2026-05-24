import { useState } from 'react'
import { useParams } from 'react-router'
import DevicePageHeader from '../components/DevicePageHeader'
import { useConnection } from '../contexts/ConnectionContext'

const inputCls = 'border border-grey-300 px-3 py-2 text-sm w-full bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'
const btnCls =
  'bg-syn-red text-white px-4 py-2 text-xs hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

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

export default function ParametersPage() {
  const { deviceId } = useParams()
  const { api } = useConnection()
  const slavePosition = Number(deviceId)

  const [index, setIndex] = useState('')
  const [subindex, setSubindex] = useState('0')
  const [uploading, setUploading] = useState(false)
  const [result, setResult] = useState<number[] | null>(null)
  const [error, setError] = useState<string | null>(null)

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

      </div>
    </div>
  )
}
