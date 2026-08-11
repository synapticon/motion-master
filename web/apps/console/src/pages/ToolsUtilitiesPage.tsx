import { useState } from 'react'
import {
  apiErrorMessage,
  type FirmwarePackageName,
  type HardwareDescription,
} from '@synapticon/motion-master-client'
import Callout from '../components/Callout'
import FieldRow from '../components/FieldRow'
import FilePickerButton from '../components/FilePickerButton'
import HardwareDescriptionView from '../components/HardwareDescriptionView'
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
      setError(apiErrorMessage(err, 'Could not decode that filename.'))
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
          <FieldRow label="Hardware" value={result.hardwareName} />
          <FieldRow
            label="Full firmware descriptor"
            value={result.fullFirmwareDescriptor}
            hint="the hardware this is built for"
          />
          <FieldRow label="Software" value={result.softwareName} />
          <FieldRow label="Software version" value={result.softwareVersion} />
          {result.firmwareId !== undefined ? (
            <>
              <FieldRow label="Firmware ID" value={result.firmwareId} hint="the hardware product" />
              <FieldRow
                label="Firmware version"
                value={String(result.firmwareVersion)}
                hint="hardware revision"
              />
              {result.buildDescriptor !== undefined && (
                <FieldRow label="Build descriptor" value={result.buildDescriptor} />
              )}
              {result.keyId !== undefined && (
                <FieldRow label="Key ID" value={result.keyId} hint="firmware encryption key" />
              )}
              {result.fieldbusProtocol !== undefined && (
                <FieldRow
                  label="Fieldbus protocol"
                  value={result.fieldbusProtocol}
                  hint={result.fieldbusProtocol === '1' ? 'EtherCAT' : 'see the specification'}
                />
              )}
            </>
          ) : (
            <p className="text-xs text-grey-500 pt-2">
              The full firmware descriptor is not the numeric{' '}
              <span className="font-mono">firmwareId-firmwareVersion-keyId-fieldbusProtocol</span>{' '}
              form, so there is nothing further to decode. That is a valid descriptor — the naming
              specification allows an arbitrary string — and the package installs either way.
            </p>
          )}
        </div>
      )}
    </section>
  )
}

/**
 * Decodes a `.hardware_description` file.
 *
 * A file rather than a filename, but small enough to belong here beside its sibling: a device's
 * hardware description is a few hundred bytes of JSON, and the answer is one table. The interesting
 * part is the build descriptor on each product — with the device's key id appended, that is what
 * decides which firmware package the hardware takes.
 */
function HardwareDescriptionTool() {
  const { api } = useConnection()
  const [result, setResult] = useState<HardwareDescription | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [parsing, setParsing] = useState(false)

  async function parse(file: File) {
    setParsing(true)
    setError(null)
    setResult(null)
    try {
      const res = await api.parseHardwareDescription(await file.text())
      setResult(res.data)
    } catch (err) {
      setError(apiErrorMessage(err, 'Could not parse that file.'))
    } finally {
      setParsing(false)
    }
  }

  return (
    <section className={cardCls}>
      <div>
        <h3 className="font-display uppercase text-sm tracking-wide">Hardware description</h3>
        <p className="text-xs text-grey-500 mt-1">
          Decodes a <span className="font-mono">.hardware_description</span> file — what a SOMANET
          product says it is, and the build descriptors that decide which firmware belongs on it. To
          read one off a connected device instead, use its Files page.
        </p>
      </div>

      {/* No `accept`: the file is named `.hardware_description`, an extension no dialog filter
          matches, so restricting by type would hide the only file anyone wants to pick. */}
      <FilePickerButton accept="" onFile={file => void parse(file)} className="h-[38px]">
        {parsing ? 'Parsing…' : 'Choose file…'}
      </FilePickerButton>

      {error && <Callout variant="error">{error}</Callout>}

      {result && (
        <div className="border-t border-grey-200 pt-3">
          <HardwareDescriptionView description={result} />
        </div>
      )}
    </section>
  )
}

/**
 * A home for small, device-free utilities.
 *
 * The other Tools pages each exist for one substantial thing (an ESI file, an SII image, a variant
 * file). This one is deliberately a page of *several* small tools instead, so a
 * one-input-one-answer helper has somewhere to live without earning a sidebar entry of its own.
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
        <HardwareDescriptionTool />
      </div>
    </div>
  )
}
