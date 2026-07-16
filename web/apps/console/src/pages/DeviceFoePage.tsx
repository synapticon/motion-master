import { useState } from 'react'
import { useParams } from 'react-router'
import { useQuery } from '@tanstack/react-query'
import DevicePageHeader from '../components/DevicePageHeader'
import HexViewer from '../components/HexViewer'
import { useConnection } from '../contexts/ConnectionContext'
import { downloadBytes } from '../utils/download'
import { parseSomanetFileList, type SomanetFile } from '@synapticon/motion-master-client'

const inputCls = 'border border-grey-300 px-3 py-2 text-sm w-full bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'
const btnCls =
  'bg-syn-red text-white px-4 py-2 text-xs hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'
const btnOutlineCls =
  'border border-grey-300 text-grey-700 px-4 py-2 text-xs hover:bg-grey-50 disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

// Reading this FoE pseudo-command unlocks the drive's filesystem for writing.
// DD1317 is the fixed unlock key the firmware expects.
const SOMANET_UNLOCK_COMMAND = 'fs-stackunlock=DD1317'

// Synapticon vendor ID — gates the SOMANET-specific filesystem features below.
const SYNAPTICON_VENDOR_ID = 0x000022d2

// Well-known files present on SOMANET drives, with whether each may be written
// back via FoE. `fs-getlist` is not a real file but a pseudo-command the firmware
// interprets on read (as is `fs-remove=<file>`); it is read-only.
const SOMANET_FILES: { name: string; write: boolean }[] = [
  { name: '.hardware_description', write: true },
  { name: '.factory_config', write: true },
  { name: '.assembly_config', write: true },
  { name: 'config.csv', write: true },
  { name: 'plant_model.csv', write: true },
  { name: 'ui.config.json', write: true },
  { name: 'SOMANET_CiA_402.xml.zip', write: true },
  { name: 'stack_image.svg.zip', write: true },
  { name: 'cversion', write: false },
  { name: 'bversion', write: false },
  { name: 'logging_curr.log', write: false },
  { name: 'logging_prev.log', write: false },
  { name: 'fs-getlist', write: false },
]

const SOMANET_READ_FILES = SOMANET_FILES.map(f => f.name)
const SOMANET_WRITE_FILES = SOMANET_FILES.filter(f => f.write).map(f => f.name)

// uWebSockets does not URL-decode path parameters, so encode the filename but
// keep `=` literal — `fs-remove=config.csv` must reach the backend verbatim.
function encodeFilename(name: string): string {
  return encodeURIComponent(name).replace(/%3D/g, '=')
}


// Human-readable elapsed time for a FoE transfer — sub-second in ms, otherwise seconds.
function formatDuration(ms: number): string {
  return ms < 1000 ? `${Math.round(ms)} ms` : `${(ms / 1000).toFixed(2)} s`
}

function decodeUtf8(bytes: Uint8Array): string | null {
  try {
    return new TextDecoder('utf-8', { fatal: true }).decode(bytes)
  } catch {
    return null
  }
}

function SomanetFileLinks({
  files,
  selected,
  onPick,
}: {
  files: string[]
  selected: string
  onPick: (name: string) => void
}) {
  return (
    <div>
      <p className={labelCls}>SOMANET Files</p>
      <div className="flex flex-wrap gap-2">
        {files.map(name => (
          <button
            key={name}
            onClick={() => onPick(name)}
            className={`px-2 py-1 text-xs border font-mono transition-colors cursor-pointer
              ${selected === name
                ? 'bg-grey-900 text-white border-grey-900'
                : 'border-grey-300 text-grey-700 hover:bg-grey-50'
              }`}
          >
            {name}
          </button>
        ))}
      </div>
    </div>
  )
}

export default function DeviceFoePage() {
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
  const [readMs, setReadMs] = useState<number | null>(null)
  const [result, setResult] = useState<Uint8Array | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [view, setView] = useState<'bytes' | 'text'>('bytes')

  const [files, setFiles] = useState<SomanetFile[] | null>(null)
  const [listing, setListing] = useState(false)
  const [listError, setListError] = useState<string | null>(null)
  const [removing, setRemoving] = useState<string | null>(null)

  const [writeFilename, setWriteFilename] = useState('')
  const [writeBytes, setWriteBytes] = useState<Uint8Array | null>(null)
  const [writing, setWriting] = useState(false)
  const [writeMs, setWriteMs] = useState<number | null>(null)
  const [writeOk, setWriteOk] = useState(false)
  const [writeError, setWriteError] = useState<string | null>(null)

  const [unlocking, setUnlocking] = useState(false)
  const [unlockOk, setUnlockOk] = useState(false)
  const [unlockError, setUnlockError] = useState<string | null>(null)

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
    setReadMs(null)
    const start = performance.now()
    try {
      const bytes = await readRaw(name)
      setReadMs(performance.now() - start)
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

  function pickWriteFilename(name: string) {
    setWriteFilename(name)
    setWriteOk(false)
    setWriteError(null)
  }

  async function handleFilePick(file: File | null) {
    setWriteOk(false)
    setWriteError(null)
    if (!file) {
      setWriteBytes(null)
      return
    }
    const buffer = await file.arrayBuffer()
    setWriteBytes(new Uint8Array(buffer))
    setWriteFilename(file.name)
  }

  async function handleUnlock() {
    if (unlocking) return
    setUnlocking(true)
    setUnlockOk(false)
    setUnlockError(null)
    try {
      await readRaw(SOMANET_UNLOCK_COMMAND)
      setUnlockOk(true)
    } catch (err) {
      setUnlockError(err instanceof Error ? err.message : 'Unknown error')
    } finally {
      setUnlocking(false)
    }
  }

  async function handleWrite() {
    if (!writeFilename || writeBytes === null || writing) return
    const ok = window.confirm(
      `Write ${writeBytes.length.toLocaleString()} byte(s) to "${writeFilename}" on device ${slavePosition}?\n` +
        'This overwrites any existing file with that name on the drive.',
    )
    if (!ok) return
    setWriting(true)
    setWriteOk(false)
    setWriteError(null)
    setWriteMs(null)
    const start = performance.now()
    try {
      const url = `${api.baseUrl}/api/devices/${slavePosition}/files/${encodeFilename(writeFilename)}`
      const response = await fetch(url, {
        method: 'PUT',
        body: new Blob([writeBytes as BlobPart]),
      })
      if (!response.ok) {
        const json = await response.json().catch(() => null)
        throw new Error(json?.error ?? `HTTP ${response.status}`)
      }
      setWriteMs(performance.now() - start)
      setWriteOk(true)
      if (isSynapticon && files) handleList()
    } catch (err) {
      setWriteError(err instanceof Error ? err.message : 'Unknown error')
    } finally {
      setWriting(false)
      // The write consumes the unlock — the drive re-locks afterwards.
      setUnlockOk(false)
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
      <DevicePageHeader
        slavePosition={slavePosition}
        title="FoE"
        description={
          <>
            Read and write files on the device over File-over-EtherCAT (FoE) — the mailbox protocol
            used for firmware images and, on SOMANET drives, the on-drive filesystem (configuration,
            hardware description, logs). Writing a file overwrites any existing file of the same
            name; some SOMANET files require a one-shot stack unlock immediately before each write.
          </>
        }
      />
      <div className="p-4 sm:px-8 sm:py-7 space-y-6">

        <div className="grid grid-cols-1 lg:grid-cols-2 gap-8 items-start">

        {/* Read */}
        <section>
          <p className="eyebrow mb-5">Read File</p>
          <div className="border border-grey-200 p-5 space-y-4">
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
              <SomanetFileLinks files={SOMANET_READ_FILES} selected={filename} onPick={pickFilename} />
            )}
            <button onClick={() => handleRead()} disabled={!filename || reading} className={btnCls}>
              {reading ? 'Reading…' : 'Read'}
            </button>
            {error && (
              <p className="text-xs text-status-bad font-mono">{error}</p>
            )}
          </div>
        </section>

        {/* Write */}
        <section>
          <p className="eyebrow mb-5">Write File</p>
          <div className="border border-grey-200 p-5 space-y-4">
            {isSynapticon && (
              <div className="border-b border-grey-100 pb-4">
                <label className={labelCls}>Stack Unlock</label>
                <button onClick={handleUnlock} disabled={unlocking} className={btnOutlineCls}>
                  {unlocking ? 'Unlocking…' : 'Unlock for Writing'}
                </button>
                <p className="text-xs text-grey-500 mt-2">
                  Some files are locked. The unlock applies to a single write only — the drive
                  re-locks afterwards, so unlock immediately before each write.
                </p>
                {unlockError && (
                  <p className="text-xs text-status-bad font-mono mt-2">{unlockError}</p>
                )}
                {unlockOk && (
                  <p className="text-xs text-status-good font-mono mt-2">
                    Unlocked — the next write is permitted.
                  </p>
                )}
              </div>
            )}
            <div>
              <label className={labelCls}>Filename</label>
              <input
                type="text"
                value={writeFilename}
                onChange={e => pickWriteFilename(e.target.value)}
                placeholder="e.g. config.csv"
                className={inputCls}
              />
            </div>
            {isSynapticon && (
              <SomanetFileLinks
                files={SOMANET_WRITE_FILES}
                selected={writeFilename}
                onPick={pickWriteFilename}
              />
            )}
            <div>
              <label className={labelCls}>Contents</label>
              <input
                type="file"
                onChange={e => handleFilePick(e.target.files?.[0] ?? null)}
                className="text-xs text-grey-700 w-full file:mr-3 file:border file:border-grey-300 file:bg-grey-50 file:px-3 file:py-1.5 file:text-xs file:cursor-pointer hover:file:bg-grey-100"
              />
              <p className="text-xs text-grey-500 mt-1 font-mono">
                {writeBytes === null
                  ? 'Choose a file to upload.'
                  : `${writeBytes.length.toLocaleString()} byte(s) ready.`}
              </p>
            </div>
            <button
              onClick={handleWrite}
              disabled={!writeFilename || writeBytes === null || writing}
              className={btnCls}
            >
              {writing ? 'Writing…' : 'Write'}
            </button>
            {writeError && (
              <p className="text-xs text-status-bad font-mono">{writeError}</p>
            )}
            {writeOk && (
              <p className="text-xs text-status-good font-mono">
                Wrote {writeBytes?.length.toLocaleString() ?? 0} byte(s) to {writeFilename}
                {writeMs !== null && ` in ${formatDuration(writeMs)}`}.
              </p>
            )}
          </div>
        </section>

        </div>

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
              <span className="text-xs text-grey-500 font-mono">
                {result.length.toLocaleString()} bytes
                {readMs !== null && ` · read in ${formatDuration(readMs)}`}
              </span>
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
                <HexViewer bytes={result.subarray(0, 256)} offsetDigits={8} />
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
