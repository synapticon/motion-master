import { useState } from 'react'
import type { FirmwarePackageName } from '@synapticon/motion-master-client'
import Callout from '../components/Callout'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { btnPrimary } from '../utils/styles'

// One shared control height for every interactive control, matching the device pages: this theme's
// spacing scale is geometric (h-9 = 6rem = 96px!), so the height has to be explicit px.
const inputCls = 'border border-grey-300 px-3 h-[38px] text-sm w-full bg-white font-mono'
const labelCls = 'text-[10px] uppercase tracking-wide text-grey-500 font-display block'
const cardCls = 'border border-grey-200 bg-white p-5 space-y-4'

const EXAMPLE_PACKAGE = 'package_SOMANET-Circulo-7_8500-04-2332_motion-drive_v5.6.10.zip'

/**
 * One row of a decoded result. `hint` explains what the field is for rather than repeating it —
 * these names come from a specification most people have not read.
 */
function Field({ label, value, hint }: { label: string; value: string; hint?: string }) {
  return (
    <div className="grid grid-cols-[11rem_1fr] gap-3 py-1.5 border-b border-grey-100 last:border-0">
      <div className={labelCls}>{label}</div>
      <div className="text-sm">
        <span className="font-mono">{value}</span>
        {hint && <span className="text-grey-400 text-xs"> — {hint}</span>}
      </div>
    </div>
  )
}

/**
 * Decodes a SOMANET firmware package filename.
 *
 * Decoded on the server rather than here, deliberately: it is the same function firmware
 * installation uses to decide whether a package can be cached, so what this reports is what an
 * install will actually do. A second implementation in TypeScript would be a second grammar to keep
 * in step, and it would disagree first on exactly the odd names worth checking.
 */
function FirmwarePackageNameTool() {
  const { api } = useConnection()
  const [filename, setFilename] = useState('')
  const [result, setResult] = useState<FirmwarePackageName | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [decoding, setDecoding] = useState(false)

  async function decode(name: string) {
    const trimmed = name.trim()
    if (trimmed === '') {
      return
    }
    setDecoding(true)
    setError(null)
    setResult(null)
    try {
      const res = await api.getFirmwarePackageName({ filename: trimmed })
      setResult(res.data)
    } catch (err) {
      // The generated client throws the parsed response body on a non-2xx, so a 400 arrives as
      // `{ error: "..." }` — a message naming which part of the grammar the name missed, which is
      // the useful half of a negative answer and belongs on screen rather than swallowed.
      const message =
        typeof err === 'object' && err !== null && 'error' in err
          ? String((err as { error: unknown }).error)
          : err instanceof Error
            ? err.message
            : 'Could not decode that filename.'
      setError(message)
    } finally {
      setDecoding(false)
    }
  }

  return (
    <section className={cardCls}>
      <div>
        <h3 className="font-display uppercase text-sm tracking-wide">Firmware package name</h3>
        <p className="text-xs text-grey-500 mt-1">
          Breaks a SOMANET firmware package filename into its five fields, and decodes the full
          firmware descriptor where it is the numeric kind. Useful for checking which hardware and
          software version a downloaded package is for without opening it — and for confirming a
          name will be recognised, since a package is only cached under a name that decodes.
        </p>
      </div>

      <form
        className="flex items-end gap-3"
        onSubmit={e => {
          e.preventDefault()
          void decode(filename)
        }}
      >
        <div className="flex-1 space-y-1">
          <label htmlFor="package-filename" className={labelCls}>
            Package filename
          </label>
          <input
            id="package-filename"
            className={inputCls}
            type="text"
            spellCheck={false}
            placeholder={EXAMPLE_PACKAGE}
            value={filename}
            onChange={e => setFilename(e.target.value)}
          />
        </div>
        <button
          type="submit"
          className={`${btnPrimary} h-[38px] inline-flex items-center`}
          disabled={decoding || filename.trim() === ''}
        >
          {decoding ? 'Decoding…' : 'Decode'}
        </button>
      </form>

      <button
        type="button"
        className="text-xs text-ocean hover:underline cursor-pointer"
        onClick={() => {
          setFilename(EXAMPLE_PACKAGE)
          void decode(EXAMPLE_PACKAGE)
        }}
      >
        Try an example
      </button>

      {error && <Callout variant="error">{error}</Callout>}

      {result && (
        <div className="border-t border-grey-200 pt-3">
          <Field label="Hardware" value={result.hardwareName} />
          <Field label="Firmware ID" value={result.firmwareId} hint="full firmware descriptor" />
          <Field label="Software" value={result.firmwareName} />
          <Field label="Version" value={result.firmwareVersion} />
          {result.productId !== undefined ? (
            <>
              <Field label="Product ID" value={String(result.productId)} />
              <Field label="Product version" value={String(result.productVersion)} />
              {result.keyId !== undefined && (
                <Field
                  label="Key ID"
                  value={String(result.keyId)}
                  hint="firmware encryption key"
                />
              )}
              {result.fieldbusProtocol !== undefined && (
                <Field
                  label="Fieldbus"
                  value={String(result.fieldbusProtocol)}
                  hint={result.fieldbusProtocol === 1 ? 'EtherCAT' : 'see the specification'}
                />
              )}
            </>
          ) : (
            <p className="text-xs text-grey-500 pt-2">
              The firmware descriptor is not the numeric{' '}
              <span className="font-mono">id-version-key-fieldbus</span> form, so there is no
              product id to report. That is a valid descriptor — the naming specification allows an
              arbitrary string — and the package installs either way.
            </p>
          )}
        </div>
      )}
    </section>
  )
}

/**
 * A home for small, device-free utilities.
 *
 * The other two Tools pages each exist for one substantial thing (an ESI file, an SII image). This
 * one is deliberately a page of *several* small tools instead, so a one-input-one-answer helper has
 * somewhere to live without earning a sidebar entry of its own.
 */
export default function ToolsUtilitiesPage() {
  return (
    <div>
      <PageHeader
        eyebrow="Tools"
        title="Utilities"
        description="Small helpers that need no device and no connection to a bus — just something typed in and an answer back."
      />
      <div className="p-4 sm:px-8 sm:py-7 space-y-6 max-w-4xl">
        <FirmwarePackageNameTool />
      </div>
    </div>
  )
}
