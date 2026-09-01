import { useState } from 'react'
import { fetchEni } from '@synapticon/motion-master-client'
import EniExplainer from '../components/EniExplainer'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { downloadText } from '../utils/download'
import { btnPrimary, btnOutline } from '../utils/styles'

// Counts the devices and datagrams in an exported document, so the reader can see the export
// covered the bus it was looking at without opening the file. Counted from the text rather than
// parsed: the numbers are a receipt, and a receipt does not need a DOM.
function summarise(xml: string) {
  const count = (tag: string) => xml.split(`<${tag}>`).length - 1
  return {
    slaves: count('Slave'),
    initCmds: count('InitCmd'),
    variables: count('Variable'),
    bytes: new TextEncoder().encode(xml).length,
  }
}

export default function ToolsEniPage() {
  const { api } = useConnection()
  const [xml, setXml] = useState<string | null>(null)
  const [warnings, setWarnings] = useState(0)
  const [elapsedMs, setElapsedMs] = useState<number | null>(null)
  const [exporting, setExporting] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [showSource, setShowSource] = useState(false)

  async function handleExport() {
    setExporting(true)
    setError(null)
    const start = performance.now()
    try {
      const result = await fetchEni(api.baseUrl)
      setXml(result.xml)
      setWarnings(result.warnings)
      setElapsedMs(performance.now() - start)
    } catch (err) {
      setXml(null)
      setElapsedMs(null)
      setError(err instanceof Error ? err.message : 'Failed to export the ENI.')
    } finally {
      setExporting(false)
    }
  }

  const summary = xml ? summarise(xml) : null

  return (
    <div>
      <PageHeader
        eyebrow="Tools"
        title="ENI"
        description="Export this bus as an EtherCAT Network Information file — the vendor-neutral configuration a third-party master replays to bring the same bus up. Needs the bus in SAFE-OP or OP."
      />
      <div className="p-4 sm:px-8 sm:py-7 space-y-6">
        <EniExplainer />

        <section className="border border-grey-200 px-4 py-4 space-y-3">
          <h3 className="eyebrow">Export</h3>
          <p className="text-xs text-grey-600 leading-relaxed">
            Reads every device's static configuration, SII and PDO assignment, and writes the
            document. This drives the bus: one EEPROM read and a burst of SDO uploads per device, so
            it takes a moment on a long bus. It changes nothing on any device.
          </p>
          <div className="flex items-center justify-between gap-3 flex-wrap">
            <div className="flex items-center gap-3">
              <button
                type="button"
                onClick={handleExport}
                disabled={exporting}
                className={btnPrimary}
              >
                {exporting ? 'Exporting…' : xml ? 'Export again' : 'Export ENI'}
              </button>
              {xml && (
                <button
                  type="button"
                  onClick={() => downloadText(xml, 'motion-master-eni.xml', 'application/xml')}
                  className={btnOutline}
                >
                  Download
                </button>
              )}
            </div>
            {elapsedMs !== null && (
              <span className="text-xs text-grey-500 font-mono">{elapsedMs.toFixed(0)} ms</span>
            )}
          </div>

          {error && <p className="text-xs text-status-bad font-mono">{error}</p>}

          {summary && (
            <div className="text-xs text-grey-600 font-mono">
              {summary.slaves} {summary.slaves === 1 ? 'device' : 'devices'} ·{' '}
              {summary.initCmds} init commands · {summary.variables} process-image variables ·{' '}
              {summary.bytes} bytes
              {warnings > 0 && (
                <span className="text-status-warn">
                  {' '}
                  · {warnings} {warnings === 1 ? 'warning' : 'warnings'} (see the server log)
                </span>
              )}
            </div>
          )}
        </section>

        {xml && (
          <section className="border border-grey-200">
            <button
              type="button"
              onClick={() => setShowSource(s => !s)}
              aria-expanded={showSource}
              className="w-full flex items-center justify-between gap-3 px-4 py-3 text-left hover:bg-grey-50 transition-colors cursor-pointer"
            >
              <span className="eyebrow">Source</span>
              <span className="text-xs text-grey-500">{showSource ? 'Hide' : 'Show'}</span>
            </button>
            {showSource && (
              <pre className="border-t border-grey-200 px-4 py-4 text-xs font-mono text-grey-800 overflow-auto max-h-[32rem] whitespace-pre">
                {xml}
              </pre>
            )}
          </section>
        )}

        {!xml && !exporting && !error && (
          <p className="text-xs text-grey-500">
            Nothing exported yet. Press <strong>Export ENI</strong> to read the bus.
          </p>
        )}
      </div>
    </div>
  )
}
