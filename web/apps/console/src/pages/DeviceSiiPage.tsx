import { useState } from 'react'
import { useParams } from 'react-router'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import DevicePageHeader from '../components/DevicePageHeader'
import FilePickerButton from '../components/FilePickerButton'
import SiiExplainer from '../components/SiiExplainer'
import SiiView from '../components/SiiView'
import SiiRawView from '../components/SiiRawView'
import { useConnection } from '../contexts/ConnectionContext'
import { downloadBytes } from '../utils/download'
import { btnOutline } from '../utils/styles'

// Human-readable fetch duration: sub-second as whole milliseconds, otherwise seconds with one
// decimal (a full EEPROM read is many small transactions and can take a noticeable moment).
const formatDuration = (ms: number) => (ms < 1000 ? `${Math.round(ms)} ms` : `${(ms / 1000).toFixed(1)} s`)

export default function DeviceSiiPage() {
  const { deviceId } = useParams()
  const slavePosition = Number(deviceId)
  const { api } = useConnection()
  const queryClient = useQueryClient()
  const [showRaw, setShowRaw] = useState(false)
  const [fetchMs, setFetchMs] = useState<number | null>(null)
  const [downloading, setDownloading] = useState(false)
  const [writing, setWriting] = useState(false)
  const [writeStatus, setWriteStatus] = useState<{ ok: boolean; msg: string } | null>(null)

  const query = useQuery({
    queryKey: ['sii', slavePosition],
    queryFn: async () => {
      const start = performance.now()
      const res = await api.readSii(slavePosition)
      setFetchMs(performance.now() - start)
      return res
    },
  })

  async function fetchRaw(): Promise<Uint8Array> {
    const res = await fetch(`${api.baseUrl}/api/devices/${slavePosition}/sii`, {
      headers: { Accept: 'application/octet-stream' },
    })
    if (!res.ok) {
      throw new Error(`HTTP ${res.status}`)
    }
    return new Uint8Array(await res.arrayBuffer())
  }

  const rawQuery = useQuery({
    queryKey: ['sii-raw', slavePosition],
    queryFn: fetchRaw,
    enabled: showRaw,
  })

  async function handleDownload() {
    setDownloading(true)
    try {
      const bytes = await fetchRaw()
      downloadBytes(bytes, `sii-slave-${slavePosition}.bin`)
    } finally {
      setDownloading(false)
    }
  }

  async function handleWriteFile(file: File) {
    const bytes = new Uint8Array(await file.arrayBuffer())
    const confirmed = window.confirm(
      `Write ${bytes.length} bytes from "${file.name}" to the EEPROM of slave ${slavePosition}?\n\n` +
        `This overwrites the device's Slave Information Interface. A wrong image can leave the ` +
        `device unidentifiable until it is re-flashed. The device must be power-cycled afterwards ` +
        `to apply the change, and writing is safest while it is in INIT or PRE-OP.`,
    )
    if (!confirmed) {
      return
    }
    setWriting(true)
    setWriteStatus(null)
    try {
      const res = await fetch(`${api.baseUrl}/api/devices/${slavePosition}/sii`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/octet-stream' },
        body: bytes,
      })
      if (!res.ok) {
        const errBody = await res.json().catch(() => null)
        throw new Error(errBody?.error ?? `HTTP ${res.status}`)
      }
      setWriteStatus({
        ok: true,
        msg: `Wrote ${bytes.length} bytes from "${file.name}". Power-cycle the device to apply.`,
      })
      // The on-device identity won't change until a power cycle, but refresh the read-back so the
      // raw view reflects what is now stored.
      await queryClient.invalidateQueries({ queryKey: ['sii-raw', slavePosition] })
    } catch (err) {
      setWriteStatus({ ok: false, msg: err instanceof Error ? err.message : 'Write failed.' })
    } finally {
      setWriting(false)
    }
  }

  const sii = query.data?.data

  return (
    <div>
      <DevicePageHeader
        slavePosition={slavePosition}
        title="SII"
        description={
          <>
            Read the device's SII (Slave Information Interface) — the EEPROM the EtherCAT Slave
            Controller reads at power-up for its identity and Sync Manager / FMMU / mailbox
            configuration. Browse the parsed categories or the raw image, download it, or overwrite
            the EEPROM from a file. Writing is destructive and requires a power cycle to apply — best
            done in INIT or PRE-OP.
          </>
        }
      />
      <div className="p-4 sm:p-8 space-y-8">
        <SiiExplainer />

        <div className="flex items-center justify-between gap-3">
          <div className="flex items-center gap-3">
            <button onClick={() => setShowRaw(v => !v)} className={btnOutline}>
              {showRaw ? 'Hide raw image' : 'Show raw image'}
            </button>
            <div className="flex items-center gap-3 ml-8">
              <button onClick={handleDownload} disabled={downloading} className={btnOutline}>
                {downloading ? 'Downloading…' : 'Download SII'}
              </button>
              <FilePickerButton
                onFile={handleWriteFile}
                disabled={writing}
                title="Overwrite the device EEPROM with an SII image from a file. Destructive — requires a power cycle to apply. Best done in INIT or PRE-OP."
              >
                {writing ? 'Writing…' : 'Write SII…'}
              </FilePickerButton>
            </div>
          </div>
          <div className="flex items-center gap-3">
            {!query.isFetching && fetchMs !== null && (
              <span className="text-xs text-grey-500" title="Time to read and parse the EEPROM">
                Loaded in {formatDuration(fetchMs)}
              </span>
            )}
            <button
              onClick={() => query.refetch()}
              disabled={query.isFetching}
              className={btnOutline}
            >
              {query.isFetching ? 'Loading…' : 'Refresh'}
            </button>
          </div>
        </div>

        {writeStatus && (
          <p className={`text-xs font-mono ${writeStatus.ok ? 'text-status-good' : 'text-status-bad'}`}>
            {writeStatus.msg}
          </p>
        )}

        {query.isError && (
          <p className="text-xs text-status-bad font-mono">
            Failed to read the SII. The device must be present and reachable (INIT or PRE-OP).
          </p>
        )}

        {query.isFetching && !sii && <p className="text-xs text-grey-600">Reading EEPROM…</p>}

        {showRaw && rawQuery.isPending && <p className="text-xs text-grey-600">Loading raw image…</p>}
        {showRaw && rawQuery.isError && (
          <p className="text-xs text-status-bad font-mono">Failed to read raw EEPROM image.</p>
        )}
        {showRaw && rawQuery.data && <SiiRawView bytes={rawQuery.data} />}

        {sii && <SiiView sii={sii} />}
      </div>
    </div>
  )
}
