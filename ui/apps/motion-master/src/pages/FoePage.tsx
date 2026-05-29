import { useState } from 'react'
import { useParams } from 'react-router'
import { useQuery } from '@tanstack/react-query'
import DevicePageHeader from '../components/DevicePageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { downloadBytes } from '../utils/download'
import { parseSomanetFileList, type SomanetFile } from '../utils/somanet'

const inputCls = 'border border-grey-300 px-3 py-2 text-sm w-full bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'
const btnCls =
  'bg-syn-red text-white px-4 py-2 text-xs hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

// Synapticon vendor ID — gates the SOMANET-specific filesystem features below.
const SYNAPTICON_VENDOR_ID = 0x000022d2

// Well-known files present on SOMANET drives. `fs-getlist` and `fs-remove=<file>`
// are not real files but FoE pseudo-commands the firmware interprets on read.
const SOMANET_FILES = [
  '.hardware_description',
  'config.csv',
  'cversion',
  'bversion',
  'fs-getlist',
  'logging_curr.log',
  'logging_prev.log',
  'plant_model.csv',
  'SOMANET_CiA_402.xml.zip',
  'stack_image.svg.zip',
  'ui.config.json',
]

// uWebSockets does not URL-decode path parameters, so encode the filename but
// keep `=` literal — `fs-remove=config.csv` must reach the backend verbatim.
function encodeFilename(name: string): string {
  return encodeURIComponent(name).replace(/%3D/g, '=')
}

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

  const devicesQuery = useQuery({
    queryKey: ['devices'],
    queryFn: () => api.getDevices(),
    staleTime: Infinity,
  })
  const device = devicesQuery.data?.data.find(d => d.slavePosition === slavePosition)
  const isSynapticon = device?.vendorId === SYNAPTICON_VENDOR_ID

  const [filename, setFilename] = useState('')
  const [reading, setReading] = useState(false)
  const [result, setResult] = useState<Uint8Array | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [view, setView] = useState<'bytes' | 'text'>('bytes')

  const [files, setFiles] = useState<SomanetFile[] | null>(null)
  const [listing, setListing] = useState(false)
  const [listError, setListError] = useState<string | null>(null)
  const [removing, setRemoving] = useState<string | null>(null)

  // Read raw bytes for an arbitrary FoE filename, throwing on a non-OK response.
  async function readRaw(name: string): Promise<Uint8Array> {
    const url = `${api.baseUrl}/api/devices/${slavePosition}/files/${encodeFilename(name)}`
    const response = await fetch(url)
    if (!response.ok) {
      const json = await response.json().catch(() => null)
      throw new Error(json?.error ?? `HTTP ${response.status}`)
    }
    return new Uint8Array(await response.arrayBuffer())
  }

  async function handleRead(name: string = filename) {
    if (!name || reading) return
    setFilename(name)
    setReading(true)
    setResult(null)
    setError(null)
    try {
      const bytes = await readRaw(name)
      setResult(bytes)
      setView('bytes')
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Unknown error')
    } finally {
      setReading(false)
    }
  }

  function pickFilename(name: string) {
    setFilename(name)
    setResult(null)
    setError(null)
  }

  async function handleList() {
    if (listing) return
    setListing(true)
    setListError(null)
    try {
      const bytes = await readRaw('fs-getlist')
      const text = new TextDecoder('utf-8').decode(bytes)
      setFiles(parseSomanetFileList(text))
    } catch (err) {
      setFiles(null)
      setListError(err instanceof Error ? err.message : 'Unknown error')
    } finally {
      setListing(false)
    }
  }

  async function handleDownload(name: string) {
    try {
      const bytes = await readRaw(name)
      downloadBytes(bytes, name)
    } catch (err) {
      setListError(err instanceof Error ? err.message : 'Unknown error')
    }
  }

  async function handleRemove(name: string) {
    if (removing) return
    const ok = window.confirm(
      `Remove "${name}" from device ${slavePosition}?\nThis permanently deletes the file from the drive.`,
    )
    if (!ok) return
    setRemoving(name)
    setListError(null)
    try {
      await readRaw(`fs-remove=${name}`)
      await handleList()
    } catch (err) {
      setListError(err instanceof Error ? err.message : 'Unknown error')
    } finally {
      setRemoving(null)
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
                onChange={e => pickFilename(e.target.value)}
                onKeyDown={e => { if (e.key === 'Enter') handleRead() }}
                placeholder="e.g. firmware.bin"
                className={inputCls}
              />
            </div>
            {isSynapticon && (
              <div>
                <p className={labelCls}>SOMANET Files</p>
                <div className="flex flex-wrap gap-2">
                  {SOMANET_FILES.map(name => (
                    <button
                      key={name}
                      onClick={() => pickFilename(name)}
                      className={`px-2 py-1 text-xs border font-mono transition-colors cursor-pointer
                        ${filename === name
                          ? 'bg-grey-900 text-white border-grey-900'
                          : 'border-grey-300 text-grey-700 hover:bg-grey-50'
                        }`}
                    >
                      {name}
                    </button>
                  ))}
                </div>
              </div>
            )}
            <button onClick={() => handleRead()} disabled={!filename || reading} className={btnCls}>
              {reading ? 'Reading…' : 'Read'}
            </button>
            {error && (
              <p className="text-xs text-status-bad font-mono">{error}</p>
            )}
          </div>
        </section>

        {isSynapticon && (
          <section>
            <div className="flex items-center gap-4 mb-5">
              <p className="eyebrow">Files on Drive</p>
              <button onClick={handleList} disabled={listing} className={btnCls}>
                {listing ? 'Listing…' : files ? 'Refresh' : 'List Files'}
              </button>
            </div>

            {listError && (
              <p className="text-xs text-status-bad font-mono mb-4">{listError}</p>
            )}

            {files && (
              files.length === 0 ? (
                <p className="text-xs text-grey-600">No files on drive.</p>
              ) : (
                <div className="border border-grey-200 overflow-x-auto">
                  <table className="w-full text-xs border-collapse">
                    <thead>
                      <tr className="border-b border-grey-200 bg-grey-50">
                        {['#', 'Filename', 'Size', 'Actions'].map(h => (
                          <th key={h} className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">
                            {h}
                          </th>
                        ))}
                      </tr>
                    </thead>
                    <tbody>
                      {files.map((file, i) => (
                        <tr key={file.name} className="border-b border-grey-100 last:border-0 hover:bg-grey-50">
                          <td className="px-4 py-2 font-mono text-grey-500">{i + 1}</td>
                          <td className="px-4 py-2 font-mono">{file.name}</td>
                          <td className="px-4 py-2 font-mono text-grey-600 whitespace-nowrap">
                            {file.size === null ? '—' : `${file.size.toLocaleString()} B`}
                          </td>
                          <td className="px-4 py-2">
                            <div className="flex gap-4">
                              <button onClick={() => handleRead(file.name)} className="text-ocean hover:underline cursor-pointer">
                                View
                              </button>
                              <button onClick={() => handleDownload(file.name)} className="text-ocean hover:underline cursor-pointer">
                                Download
                              </button>
                              <button
                                onClick={() => handleRemove(file.name)}
                                disabled={removing === file.name}
                                className="text-syn-red hover:underline cursor-pointer disabled:opacity-50 disabled:cursor-not-allowed"
                              >
                                {removing === file.name ? 'Removing…' : 'Remove'}
                              </button>
                            </div>
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              )
            )}
          </section>
        )}

        {result && (
          <section>
            <div className="flex items-center gap-4 mb-4">
              <p className="eyebrow">Result</p>
              <span className="text-xs text-grey-500 font-mono">{result.length.toLocaleString()} bytes</span>
              <button onClick={() => downloadBytes(result, filename)} className={btnCls}>
                Download
              </button>
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
