import { useState } from 'react'
import type { SlaveInformationInterface } from '@synapticon/motion-master-client'
import FilePickerButton from '../components/FilePickerButton'
import PageHeader from '../components/PageHeader'
import SiiExplainer from '../components/SiiExplainer'
import SiiView from '../components/SiiView'
import SiiRawView from '../components/SiiRawView'
import { useConnection } from '../contexts/ConnectionContext'

export default function ToolsSiiPage() {
  const { api } = useConnection()
  const [filename, setFilename] = useState<string | null>(null)
  const [bytes, setBytes] = useState<Uint8Array | null>(null)
  const [sii, setSii] = useState<SlaveInformationInterface | null>(null)
  const [parsing, setParsing] = useState(false)
  const [error, setError] = useState<string | null>(null)

  async function loadFile(file: File) {
    setParsing(true)
    setError(null)
    setSii(null)
    try {
      const raw = new Uint8Array(await file.arrayBuffer())
      setBytes(raw)
      setFilename(file.name)
      // Parse through the backend so the same C++ decoder the device read path uses produces the
      // structure — no second parser to keep in sync. The generated client JSON-encodes bodies, so
      // post the raw bytes directly.
      const res = await fetch(`${api.baseUrl}/api/sii/parse`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/octet-stream' },
        body: raw,
      })
      if (!res.ok) {
        const body = await res.json().catch(() => null)
        throw new Error(body?.error ?? `HTTP ${res.status}`)
      }
      setSii((await res.json()) as SlaveInformationInterface)
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to parse the SII file.')
    } finally {
      setParsing(false)
    }
  }

  return (
    <div>
      <PageHeader
        eyebrow="Tools"
        title="SII"
        description="Load a previously downloaded SII (EEPROM) image and decode it offline — no device required. Use the Download SII button on a device's SII page to capture one."
      />
      <div className="p-4 sm:p-8 space-y-6">
        <SiiExplainer />

        <div className="flex items-center gap-3">
          <FilePickerButton onFile={loadFile} disabled={parsing}>
            {bytes ? 'Load another file' : 'Load SII file'}
          </FilePickerButton>
          {filename && (
            <span className="text-xs text-grey-500 font-mono">
              {filename}
              {bytes && ` · ${bytes.length} bytes`}
            </span>
          )}
        </div>

        {parsing && <p className="text-xs text-grey-600">Parsing…</p>}
        {error && <p className="text-xs text-status-bad font-mono">{error}</p>}

        {!bytes && !parsing && (
          <p className="text-xs text-grey-500">
            No file loaded. Choose a <code>.bin</code> SII image to decode it.
          </p>
        )}

        {bytes && <SiiRawView bytes={bytes} />}
        {sii && <SiiView sii={sii} />}
      </div>
    </div>
  )
}
