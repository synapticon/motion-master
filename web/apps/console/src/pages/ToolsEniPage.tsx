import { useState } from 'react'
import { fetchEni, parseEni, type EniParseResult } from '@synapticon/motion-master-client'
import EniExplainer from '../components/EniExplainer'
import EniView from '../components/EniView'
import FilePickerButton from '../components/FilePickerButton'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { downloadText } from '../utils/download'
import { btnPrimary, btnOutline } from '../utils/styles'

/// Where the document on screen came from. It matters to the reader: an exported document describes
/// the bus in front of them, a loaded one describes somebody else's.
type Source = { kind: 'export'; warnings: number } | { kind: 'file'; filename: string }

export default function ToolsEniPage() {
  const { api } = useConnection()
  const [xml, setXml] = useState<string | null>(null)
  const [parsed, setParsed] = useState<EniParseResult | null>(null)
  const [source, setSource] = useState<Source | null>(null)
  const [elapsedMs, setElapsedMs] = useState<number | null>(null)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [showSource, setShowSource] = useState(false)

  /// Parses a document and shows it. The same view serves both sources, because what a reader wants
  /// to know about a document does not depend on who wrote it.
  async function show(document: string, from: Source) {
    setParsed(await parseEni(api.baseUrl, document))
    setXml(document)
    setSource(from)
  }

  function reset() {
    setError(null)
    setParsed(null)
    setXml(null)
    setSource(null)
    setElapsedMs(null)
  }

  async function handleExport() {
    setBusy(true)
    reset()
    const start = performance.now()
    try {
      const result = await fetchEni(api.baseUrl)
      await show(result.xml, { kind: 'export', warnings: result.warnings })
      setElapsedMs(performance.now() - start)
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to export the ENI.')
    } finally {
      setBusy(false)
    }
  }

  async function handleFile(file: File) {
    setBusy(true)
    reset()
    try {
      await show(await file.text(), { kind: 'file', filename: file.name })
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to read the ENI file.')
    } finally {
      setBusy(false)
    }
  }

  return (
    <div>
      <PageHeader
        eyebrow="Tools"
        title="ENI"
        description="Export this bus as an EtherCAT Network Information file, or read one another tool wrote. An ENI is the vendor-neutral configuration a third-party master replays to bring a bus up."
      />
      <div className="p-4 sm:px-8 sm:py-7 space-y-6">
        <EniExplainer />

        <section className="border border-grey-200 px-4 py-4 space-y-3">
          <h3 className="eyebrow">Export or load</h3>
          <p className="text-xs text-grey-600 leading-relaxed">
            Exporting reads every device's static configuration, SII and PDO assignment, and writes
            the document. It drives the bus — one EEPROM read and a burst of SDO uploads per device
            — and needs the bus in SAFE-OP or OP. It changes nothing on any device. Loading a file
            needs no bus at all, and is the way to read a configuration produced by EC-Engineer,
            TwinCAT or anything else.
          </p>
          <div className="flex items-center justify-between gap-3 flex-wrap">
            <div className="flex items-center gap-3">
              <button type="button" onClick={handleExport} disabled={busy} className={btnPrimary}>
                {busy ? 'Working…' : 'Export ENI'}
              </button>
              <FilePickerButton onFile={handleFile} disabled={busy} accept=".xml,application/xml">
                Load ENI file
              </FilePickerButton>
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

          {source && (
            <p className="text-xs text-grey-500 font-mono">
              {source.kind === 'export'
                ? `exported from this bus${source.warnings > 0 ? ` · ${source.warnings} export ${source.warnings === 1 ? 'warning' : 'warnings'} in the server log` : ''}`
                : source.filename}
              {xml && ` · ${new TextEncoder().encode(xml).length} bytes`}
            </p>
          )}
        </section>

        {parsed && <EniView parsed={parsed} />}

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

        {!xml && !busy && !error && (
          <p className="text-xs text-grey-500">
            Nothing loaded yet. Press <strong>Export ENI</strong> to read the bus, or{' '}
            <strong>Load ENI file</strong> to open one from elsewhere.
          </p>
        )}
      </div>
    </div>
  )
}
