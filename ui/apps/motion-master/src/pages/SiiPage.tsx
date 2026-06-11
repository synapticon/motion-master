import { useState } from 'react'
import { useParams } from 'react-router'
import { useQuery } from '@tanstack/react-query'
import DevicePageHeader from '../components/DevicePageHeader'
import SiiExplainer from '../components/SiiExplainer'
import SiiView from '../components/SiiView'
import SiiRawView from '../components/SiiRawView'
import { useConnection } from '../contexts/ConnectionContext'
import { downloadBytes } from '../utils/download'
import { btnOutline } from '../utils/styles'

// Human-readable fetch duration: sub-second as whole milliseconds, otherwise seconds with one
// decimal (a full EEPROM read is many small transactions and can take a noticeable moment).
const formatDuration = (ms: number) => (ms < 1000 ? `${Math.round(ms)} ms` : `${(ms / 1000).toFixed(1)} s`)

export default function SiiPage() {
  const { deviceId } = useParams()
  const slavePosition = Number(deviceId)
  const { api } = useConnection()
  const [showRaw, setShowRaw] = useState(false)
  const [fetchMs, setFetchMs] = useState<number | null>(null)
  const [downloading, setDownloading] = useState(false)

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

  const sii = query.data?.data

  return (
    <div>
      <DevicePageHeader slavePosition={slavePosition} title="SII" />
      <div className="p-4 sm:p-8 space-y-8">
        <SiiExplainer />

        <div className="flex items-center justify-end gap-3">
          {!query.isFetching && fetchMs !== null && (
            <span className="text-xs text-grey-500" title="Time to read and parse the EEPROM">
              Loaded in {formatDuration(fetchMs)}
            </span>
          )}
          <button onClick={() => query.refetch()} disabled={query.isFetching} className={btnOutline}>
            {query.isFetching ? 'Loading…' : 'Refresh'}
          </button>
          <button onClick={handleDownload} disabled={downloading} className={btnOutline}>
            {downloading ? 'Downloading…' : 'Download SII'}
          </button>
          <button onClick={() => setShowRaw(v => !v)} className={btnOutline}>
            {showRaw ? 'Hide raw image' : 'Show raw image'}
          </button>
        </div>

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
