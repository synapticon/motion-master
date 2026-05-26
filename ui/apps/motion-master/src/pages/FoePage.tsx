import { useState } from 'react'
import { useParams } from 'react-router'
import DevicePageHeader from '../components/DevicePageHeader'
import { useConnection } from '../contexts/ConnectionContext'

const inputCls = 'border border-grey-300 px-3 py-2 text-sm w-full bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'
const btnCls =
  'bg-syn-red text-white px-4 py-2 text-xs hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

function hexDump(bytes: Uint8Array): string {
  const rows: string[] = []
  const limit = Math.min(bytes.length, 256)
  for (let i = 0; i < limit; i += 16) {
    const chunk = Array.from(bytes.slice(i, Math.min(i + 16, limit)))
    const offset = i.toString(16).padStart(8, '0')
    const firstHalf = chunk.slice(0, 8).map(b => b.toString(16).padStart(2, '0').toUpperCase())
    const secondHalf = chunk.slice(8).map(b => b.toString(16).padStart(2, '0').toUpperCase())
    const hexPart = (firstHalf.join(' ').padEnd(23, ' ') + '  ' + secondHalf.join(' ')).padEnd(48, ' ')
    const ascii = chunk.map(b => b >= 32 && b < 127 ? String.fromCharCode(b) : '.').join('')
    rows.push(`${offset}  ${hexPart}  |${ascii}|`)
  }
  return rows.join('\n')
}

function decodeUtf8(bytes: Uint8Array): string | null {
  try {
    return new TextDecoder('utf-8', { fatal: true }).decode(bytes)
  } catch {
    return null
  }
}

export default function FoePage() {
  const { deviceId } = useParams()
  const { api } = useConnection()
  const slavePosition = Number(deviceId)

  const [filename, setFilename] = useState('')
  const [reading, setReading] = useState(false)
  const [result, setResult] = useState<Uint8Array | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [view, setView] = useState<'bytes' | 'text'>('bytes')

  async function handleRead() {
    if (!filename || reading) return
    setReading(true)
    setResult(null)
    setError(null)
    try {
      const url = `${api.baseUrl}/api/devices/${slavePosition}/files/${encodeURIComponent(filename)}`
      const response = await fetch(url)
      if (!response.ok) {
        const json = await response.json().catch(() => null)
        setError(json?.error ?? `HTTP ${response.status}`)
        return
      }
      const buffer = await response.arrayBuffer()
      setResult(new Uint8Array(buffer))
      setView('bytes')
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Unknown error')
    } finally {
      setReading(false)
    }
  }

  const textContent = result ? decodeUtf8(result) : null

  return (
    <div>
      <DevicePageHeader slavePosition={slavePosition} title="FoE" />
      <div className="p-4 sm:p-8 space-y-8">

        <section>
          <p className="eyebrow mb-5">Read File</p>
          <div className="border border-grey-200 p-5 max-w-xl space-y-4">
            <div>
              <label className={labelCls}>Filename</label>
              <input
                type="text"
                value={filename}
                onChange={e => { setFilename(e.target.value); setResult(null); setError(null) }}
                onKeyDown={e => { if (e.key === 'Enter') handleRead() }}
                placeholder="e.g. firmware.bin"
                className={inputCls}
              />
            </div>
            <button onClick={handleRead} disabled={!filename || reading} className={btnCls}>
              {reading ? 'Reading…' : 'Read'}
            </button>
            {error && (
              <p className="text-xs text-status-bad font-mono">{error}</p>
            )}
          </div>
        </section>

        {result && (
          <section>
            <div className="flex items-center gap-4 mb-4">
              <p className="eyebrow">Result</p>
              <span className="text-xs text-grey-500 font-mono">{result.length.toLocaleString()} bytes</span>
              <div className="flex ml-auto">
                {(['bytes', 'text'] as const).map(v => (
                  <button
                    key={v}
                    onClick={() => setView(v)}
                    disabled={v === 'text' && textContent === null}
                    title={v === 'text' && textContent === null ? 'Not valid UTF-8' : undefined}
                    className={`px-3 py-1.5 text-xs border transition-colors first:border-r-0
                      ${view === v
                        ? 'bg-grey-900 text-white border-grey-900'
                        : 'border-grey-300 text-grey-700 hover:bg-grey-50 disabled:opacity-40 disabled:cursor-not-allowed cursor-pointer'
                      }`}
                  >
                    {v === 'bytes' ? 'Bytes' : 'Text'}
                  </button>
                ))}
              </div>
            </div>

            {view === 'bytes' && (
              <div>
                <pre className="text-xs font-mono bg-grey-50 border border-grey-200 p-4 overflow-x-auto leading-5">
                  {hexDump(result)}
                </pre>
                {result.length > 256 && (
                  <p className="text-xs text-grey-500 mt-2">
                    Showing first 256 of {result.length.toLocaleString()} bytes.
                  </p>
                )}
              </div>
            )}

            {view === 'text' && textContent !== null && (
              <pre className="text-xs font-mono bg-grey-50 border border-grey-200 p-4 overflow-x-auto whitespace-pre-wrap break-words max-h-96 leading-5">
                {textContent}
              </pre>
            )}
          </section>
        )}

      </div>
    </div>
  )
}
