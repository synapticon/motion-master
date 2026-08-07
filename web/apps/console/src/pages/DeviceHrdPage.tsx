import { useMemo, useState } from 'react'
import { Link, useParams } from 'react-router'
import type uPlot from 'uplot'
import type { HrdRecording } from '@synapticon/motion-master-client'
import Checkbox from '../components/Checkbox'
import DevicePageHeader from '../components/DevicePageHeader'
import HrdExplainer from '../components/HrdExplainer'
import MonitoringChart, { SAMPLE_INDEX } from '../components/MonitoringChart'
import { WireTiming, useWireTiming } from '../components/WireTiming'
import { useConnection } from '../contexts/ConnectionContext'
import { downloadBytes } from '../utils/download'
import { formatBytes } from '../utils/format'
import { btnOutline, btnPrimary } from '../utils/styles'

const inputCls = 'border border-grey-300 px-3 h-[38px] text-sm bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'

// The two recordings the drive can make, with what each is for. The value is what the API takes;
// the label is the API's own wording, so what is read here reads the same as what was recorded.
const DATA_CHOICES = [
  { value: 'encoder-raw', label: 'Encoder raw data' },
  { value: 'system-identification', label: 'System identification data' },
] as const

type HrdData = (typeof DATA_CHOICES)[number]['value']

// Series that start hidden. `raw` is the whole 32-bit encoder word, so its scale (up to ~268
// million) flattens the two 14-bit tracks plotted beside it into a straight line; it is the column
// an algorithm wants and the one an eye does not. The legend toggles it back on.
const HIDDEN_BY_DEFAULT = ['raw']

// Surfaces the node layer's error string from a failed request (matching the other device pages).
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
  if (err instanceof Error) return err.message
  return 'Unknown error'
}

export default function DeviceHrdPage() {
  const { deviceId } = useParams()
  const slavePosition = Number(deviceId)
  const { api } = useConnection()
  const { timing, measure } = useWireTiming()

  const [data, setData] = useState<HrdData>('encoder-raw')
  const [recording, setRecording] = useState<HrdRecording | null>(null)
  const [reading, setReading] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [downloading, setDownloading] = useState(false)
  // Which columns are plotted. Empty means "not chosen yet", which is filled in from the recording
  // when one arrives — the columns are not known until then, and they differ per data selection.
  const [shown, setShown] = useState<Set<string>>(new Set())

  async function handleRead() {
    if (reading) return
    setReading(true)
    setError(null)
    try {
      const response = await measure(() => api.readHrdRecording(slavePosition, { data }))
      setRecording(response.data)
      setShown(new Set(response.data.columns))
    } catch (err) {
      setRecording(null)
      setError(apiError(err))
    } finally {
      setReading(false)
    }
  }

  // CSV comes from the server, not from the rows already on this page: it is the same read decoded
  // the other way, so a file saved here is byte-for-byte what a script fetching the endpoint gets.
  async function handleDownloadCsv() {
    if (downloading) return
    setDownloading(true)
    setError(null)
    try {
      const url = `${api.baseUrl}/api/devices/${slavePosition}/hrd?data=${data}`
      const response = await fetch(url, { headers: { Accept: 'text/csv' } })
      if (!response.ok) {
        const json = await response.json().catch(() => null)
        throw new Error(json?.error ?? `HTTP ${response.status}`)
      }
      const bytes = new Uint8Array(await response.arrayBuffer())
      downloadBytes(bytes, `hrd-slave-${slavePosition}-${data}.csv`)
    } catch (err) {
      setError(apiError(err))
    } finally {
      setDownloading(false)
    }
  }

  // uPlot data for the chosen columns: x is the sample number (the samples are evenly spaced and
  // the recording carries no timestamps), then one array per selected column.
  const chart = useMemo(() => {
    const empty = { data: [[]] as unknown as uPlot.AlignedData, labels: [] as string[] }
    if (!recording || recording.samples.length === 0) return empty
    const columns = recording.columns
      .map((name, i) => ({ name, i }))
      .filter(({ name }) => shown.has(name))
    if (columns.length === 0) return empty
    const xs = recording.samples.map((_, i) => i)
    const ys = columns.map(({ i }) => recording.samples.map((row) => row[i]))
    return { data: [xs, ...ys] as uPlot.AlignedData, labels: columns.map((c) => c.name) }
  }, [recording, shown])

  function toggle(column: string) {
    setShown((current) => {
      const next = new Set(current)
      if (next.has(column)) {
        next.delete(column)
      } else {
        next.add(column)
      }
      return next
    })
  }

  return (
    <div>
      <DevicePageHeader
        slavePosition={slavePosition}
        title="HRD"
        description="Read back and plot a high resolution data recording the drive made into its own flash."
      />

      <div className="px-8 py-6 space-y-6">
        <HrdExplainer />

        <section className="border border-grey-200 p-4 space-y-4">
          <h3 className="font-display uppercase text-sm tracking-wide">Read a recording</h3>
          <p className="text-xs text-grey-600 max-w-3xl">
            Fetches the drive's <code>hr_data</code> files over FoE and decodes them. Choose the same
            data the recording was made with — nothing on the drive says which it holds, so the wrong
            choice decodes the same bytes into different, plausible-looking numbers. Recording itself
            is the{' '}
            <Link
              to={`/devices/${slavePosition}/procedures/hrd-streaming`}
              className="text-ocean hover:underline"
            >
              HRD streaming procedure
            </Link>
            ; a drive that has never recorded holds no files to read.
          </p>

          <div className="flex flex-wrap items-end justify-between gap-4">
            <div className="flex flex-wrap items-end gap-3">
              <div>
                <label htmlFor="hrd-data" className={labelCls}>
                  Data
                </label>
                <select
                  id="hrd-data"
                  className={inputCls}
                  value={data}
                  onChange={(e) => setData(e.target.value as HrdData)}
                  disabled={reading}
                >
                  {DATA_CHOICES.map((choice) => (
                    <option key={choice.value} value={choice.value}>
                      {choice.label}
                    </option>
                  ))}
                </select>
              </div>
              <button
                onClick={handleRead}
                disabled={reading}
                className={`${btnPrimary} h-[38px] inline-flex items-center`}
              >
                {reading ? 'Reading…' : 'Read Recording'}
              </button>
              <button
                onClick={handleDownloadCsv}
                disabled={downloading || reading}
                className={`${btnOutline} h-[38px] inline-flex items-center`}
              >
                {downloading ? 'Downloading…' : 'Download CSV'}
              </button>
            </div>
            <WireTiming label="HRD read" timing={timing} />
          </div>

          {error && <p className="text-xs text-syn-red">{error}</p>}
        </section>

        {recording && (
          <section className="border border-grey-200 p-4 space-y-4">
            <h3 className="font-display uppercase text-sm tracking-wide">Recording</h3>

            <dl className="flex flex-wrap gap-x-8 gap-y-2 text-sm">
              {[
                { label: 'Samples', value: recording.sampleCount.toLocaleString() },
                { label: 'Bytes read', value: formatBytes(recording.byteCount) },
                { label: 'Files', value: String(recording.files.length) },
                {
                  label: 'Trailing',
                  value: recording.trailingBytes === 0 ? '—' : `${recording.trailingBytes} B`,
                },
              ].map((item) => (
                <div key={item.label}>
                  <dt className="eyebrow">{item.label}</dt>
                  <dd className="font-mono">{item.value}</dd>
                </div>
              ))}
            </dl>

            <p className="text-xs text-grey-600 font-mono">
              {recording.files
                .map((f) => (f.byteCount === undefined ? f.name : `${f.name} (${f.byteCount} B)`))
                .join('  ·  ')}
            </p>

            {recording.samples.length === 0 ? (
              <p className="text-xs text-grey-600">
                The files hold no complete sample. A recording that was cancelled almost immediately
                looks like this — the drive discards whatever it had buffered but not yet written.
              </p>
            ) : (
              <>
                <div className="flex flex-wrap gap-4">
                  {recording.columns.map((column) => (
                    <label
                      key={column}
                      className="flex items-center gap-2 text-sm cursor-pointer"
                    >
                      <Checkbox checked={shown.has(column)} onChange={() => toggle(column)} />
                      <span className="font-mono">{column}</span>
                    </label>
                  ))}
                </div>
                <MonitoringChart
                  data={chart.data}
                  labels={chart.labels}
                  hidden={HIDDEN_BY_DEFAULT}
                  xAxis={SAMPLE_INDEX}
                />
              </>
            )}
          </section>
        )}
      </div>
    </div>
  )
}
