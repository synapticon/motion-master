import { useEffect, useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import {
  apiErrorMessage,
  type EncoderConfiguration,
  type IcMuEepromByte,
  type IcMuI2CTransfer,
  type IcMuKeyRegister,
  type ProcedureSnapshot,
} from '@synapticon/motion-master-client'
import { useConnection } from '../contexts/ConnectionContext'
import { downloadText } from '../utils/download'
import { btnOutline } from '../utils/styles'

const KEY_REGISTERS = 'ic-mu-key-registers'
const EXPORT = 'ic-mu-eeprom-export'
const I2C_REGISTER = 'ic-mu-i2c-register'
const RESTORE_FILE = 'ic-mu-restore-file'
const ERASE = 'ic-mu-eeprom-erase'

const inputCls = 'border border-grey-300 px-3 h-[38px] text-sm bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'

type Target = 'eeprom' | 'ic-pvl' | 'ic-pvl-status'

function hex(n: number): string {
  return `0x${n.toString(16).toUpperCase().padStart(2, '0')}`
}

function stepValue<T>(snapshot: ProcedureSnapshot | undefined, id: string): T | undefined {
  return snapshot?.steps.find(step => step.id === id)?.value as T | undefined
}

// The error of the first failed step, which says why a run failed.
function failure(snapshot: ProcedureSnapshot | undefined): string | undefined {
  return snapshot?.steps.find(step => step.status === 'failed')?.error ?? undefined
}

// A byte typed as decimal or with a 0x prefix, or nothing for anything else.
function parseByte(text: string): number | undefined {
  const trimmed = text.trim()
  const value = /^0x[0-9a-f]{1,2}$/i.test(trimmed)
    ? parseInt(trimmed.slice(2), 16)
    : /^\d{1,3}$/.test(trimmed)
      ? Number(trimmed)
      : NaN
  return Number.isInteger(value) && value >= 0 && value <= 255 ? value : undefined
}

function eepromCsv(bytes: IcMuEepromByte[]): string {
  return ['address,value', ...bytes.map(b => `${hex(b.address)},${hex(b.value)}`)].join('\n') + '\n'
}

// One procedure of this section: its last snapshot, and a way to start it. A run of another
// encoder is not this card's, so `mine` says whether the snapshot belongs to it.
function useEncoderProcedure(
  slavePosition: number,
  name: string,
  encoderOrdinal: EncoderConfiguration['encoderOrdinal'],
) {
  const { api } = useConnection()
  const queryClient = useQueryClient()
  const key = ['procedure', slavePosition, name]
  const snapshot = useQuery({
    queryKey: key,
    queryFn: () => api.getProcedure(slavePosition, name).then(r => r.data),
    refetchInterval: query => (query.state.data?.status === 'running' ? 300 : false),
    refetchIntervalInBackground: true,
    retry: false,
  }).data
  const start = useMutation({
    mutationFn: (body: Record<string, unknown>) =>
      api.startProcedure(slavePosition, name, { encoderOrdinal, ...body }),
    onSettled: () => queryClient.invalidateQueries({ queryKey: key }),
  })
  const plan = stepValue<{ encoderOrdinal: number }>(snapshot, 'validate')
  const mine =
    snapshot !== undefined && snapshot.status !== 'idle' && plan?.encoderOrdinal === encoderOrdinal
  return {
    snapshot: mine ? snapshot : undefined,
    running: snapshot?.status === 'running',
    start,
  }
}

/**
 * The EEPROM tools of one iC-MU encoder: its key registers, an export and an erase of its EEPROM,
 * the restore file, and one byte of the EEPROM or the iC-PVL over the I²C bridge.
 */
export default function IcMuEepromSection({
  slavePosition,
  encoder,
  busy,
}: {
  slavePosition: number
  encoder: EncoderConfiguration
  busy: boolean
}) {
  const ordinal = encoder.encoderOrdinal
  const restoreFilePort = encoder.sensorPort === 1 || encoder.sensorPort === 2
  const keyRegisters = useEncoderProcedure(slavePosition, KEY_REGISTERS, ordinal)
  const exported = useEncoderProcedure(slavePosition, EXPORT, ordinal)
  const i2c = useEncoderProcedure(slavePosition, I2C_REGISTER, ordinal)
  const restoreFile = useEncoderProcedure(slavePosition, RESTORE_FILE, ordinal)
  const erase = useEncoderProcedure(slavePosition, ERASE, ordinal)
  const running = [keyRegisters, exported, i2c, restoreFile, erase].some(p => p.running)
  const disabled = busy || running

  const [target, setTarget] = useState<Target>('eeprom')
  const [address, setAddress] = useState('0x40')
  const [value, setValue] = useState('0x00')

  // An export started here saves its CSV when it finishes. One read on page load does not.
  const [exportRequested, setExportRequested] = useState(false)
  const exportBytes = stepValue<IcMuEepromByte[]>(exported.snapshot, 'read-eeprom')
  useEffect(() => {
    if (exportRequested && exported.snapshot?.status === 'succeeded' && exportBytes) {
      downloadText(
        eepromCsv(exportBytes),
        `ic-mu-eeprom-encoder-${ordinal}.csv`,
        'text/csv',
      )
      setExportRequested(false)
    } else if (exportRequested && exported.snapshot?.status === 'failed') {
      setExportRequested(false)
    }
  }, [exportRequested, exported.snapshot?.status, exportBytes, ordinal])

  const registers = stepValue<IcMuKeyRegister[]>(keyRegisters.snapshot, 'read-key-registers')
  const transfer = stepValue<IcMuI2CTransfer>(i2c.snapshot, 'i2c-transfer')
  const restoreFileName = stepValue<{ file: string }>(restoreFile.snapshot, 'restore-file')?.file
  const parsedAddress = parseByte(address)
  const parsedValue = parseByte(value)

  const onErase = () => {
    const confirmed = window.confirm(
      `Erase EEPROM 0x00-0x4C of encoder ${ordinal}?\n\n` +
        'This removes the configuration and the calibration of the iC-MU and the configuration of ' +
        'the iC-PVL from the EEPROM' +
        (restoreFilePort ? ', and deletes the restore file of the sensor port' : '') +
        '. After the next power cycle the encoder reports no valid position until it is ' +
        'configured and calibrated again. Export the EEPROM first if you may want it back.',
    )
    if (confirmed) {
      erase.start.mutate({})
    }
  }

  const errors = [
    keyRegisters.start.error,
    exported.start.error,
    i2c.start.error,
    restoreFile.start.error,
    erase.start.error,
  ].filter(Boolean)

  return (
    <div className="px-5 py-4 border-t border-grey-100 space-y-4">
      <h4 className="font-display uppercase tracking-wide text-xs text-grey-700">EEPROM</h4>
      <p className="text-xs text-grey-600 max-w-3xl">
        The iC-MU and the iC-PVL load their configuration from one EEPROM at start-up. The iC-MU
        reaches it, and the iC-PVL, through its own I²C bridge. While the bridge is in use the
        encoder reports no position, so the drive must not be in Operation Enabled.
      </p>

      <dl className="grid gap-x-4 gap-y-3 text-sm sm:grid-cols-[max-content_1fr] items-start">
        <dt>
          <button
            type="button"
            className={`${btnOutline} h-[38px] w-44`}
            disabled={disabled}
            onClick={() => keyRegisters.start.mutate({})}
          >
            Read key registers
          </button>
        </dt>
        <dd className="text-xs text-grey-600 self-center">
          Reads the 33 registers of the restore file from the RAM of the iC-MU, each with the EEPROM
          address that keeps it. Writes nothing.
        </dd>

        <dt>
          <button
            type="button"
            className={`${btnOutline} h-[38px] w-44`}
            disabled={disabled}
            onClick={() => {
              setExportRequested(true)
              exported.start.mutate({})
            }}
          >
            Export EEPROM
          </button>
        </dt>
        <dd className="text-xs text-grey-600 self-center">
          Reads EEPROM 0x00-0x4C, the configuration of the iC-MU and of the iC-PVL, and saves the
          77 bytes as a CSV file. Writes nothing to the EEPROM.
        </dd>

        {restoreFilePort && (
          <>
            <dt>
              <button
                type="button"
                className={`${btnOutline} h-[38px] w-44`}
                disabled={disabled}
                onClick={() => restoreFile.start.mutate({})}
              >
                Save restore file
              </button>
            </dt>
            <dd className="text-xs text-grey-600 self-center">
              Writes the key registers to{' '}
              {`circulo_integrated_encoder_${encoder.sensorPort}_config.bin`} and reads the file
              back. The drive writes that file into the encoder when
              the iC-MU reports a corrupted EEPROM at start-up. Save it from an encoder that works.
            </dd>
          </>
        )}

        <dt>
          <button
            type="button"
            className={`${btnOutline} h-[38px] w-44 border-status-bad text-status-bad`}
            disabled={disabled}
            onClick={onErase}
          >
            Erase EEPROM
          </button>
        </dt>
        <dd className="text-xs text-grey-600 self-center">
          Writes 0xFF to EEPROM 0x00-0x4C, checks it,
          {restoreFilePort ? ' and deletes the restore file. ' : ' '}
          It erases the configuration and the calibration. The chips keep their RAM until the next
          power cycle; after it, configure and calibrate the encoder again.
        </dd>
      </dl>

      <div className="flex flex-wrap items-end gap-3">
        <div>
          <label className={labelCls} htmlFor={`i2c-target-${ordinal}`}>
            I²C target
          </label>
          <select
            id={`i2c-target-${ordinal}`}
            className={inputCls}
            value={target}
            onChange={event => setTarget(event.target.value as Target)}
          >
            <option value="eeprom">EEPROM</option>
            <option value="ic-pvl">iC-PVL</option>
            <option value="ic-pvl-status">iC-PVL status and command</option>
          </select>
        </div>
        <div>
          <label className={labelCls} htmlFor={`i2c-address-${ordinal}`}>
            Address
          </label>
          <input
            id={`i2c-address-${ordinal}`}
            className={`${inputCls} font-mono w-24`}
            value={address}
            onChange={event => setAddress(event.target.value)}
          />
        </div>
        <div>
          <label className={labelCls} htmlFor={`i2c-value-${ordinal}`}>
            Value
          </label>
          <input
            id={`i2c-value-${ordinal}`}
            className={`${inputCls} font-mono w-24`}
            value={value}
            onChange={event => setValue(event.target.value)}
          />
        </div>
        <button
          type="button"
          className={`${btnOutline} h-[38px]`}
          disabled={disabled || parsedAddress === undefined}
          onClick={() => i2c.start.mutate({ target, write: false, registerAddress: parsedAddress })}
        >
          Read
        </button>
        <button
          type="button"
          className={`${btnOutline} h-[38px]`}
          disabled={disabled || parsedAddress === undefined || parsedValue === undefined}
          onClick={() =>
            i2c.start.mutate({ target, write: true, registerAddress: parsedAddress, value: parsedValue })
          }
        >
          Write
        </button>
        {transfer && (
          <span className="text-sm font-mono h-[38px] flex items-center">
            {transfer.target} {hex(transfer.registerAddress)} {transfer.write ? '←' : '='}{' '}
            {hex(transfer.value)}
          </span>
        )}
      </div>
      <p className="text-xs text-grey-600 max-w-3xl">
        One byte over the I²C bridge, in decimal or with a 0x prefix. A write to the EEPROM changes
        nothing in the chips until the next power cycle. A write to the RAM configuration of the
        iC-PVL sets its CFG_ERR. The iC-PVL status target reaches only the status and the command
        register, which is 0x01 there.
      </p>

      {errors.map(error => (
        <p key={String(error)} className="text-xs text-status-bad">
          {apiErrorMessage(error)}
        </p>
      ))}
      {[keyRegisters, exported, i2c, restoreFile, erase].map(({ snapshot }, index) => {
        const message = snapshot?.status === 'failed' ? failure(snapshot) : undefined
        return message ? (
          <p key={index} className="text-xs text-status-bad">
            {message}
          </p>
        ) : null
      })}
      {running && <p className="text-xs text-grey-600">Running…</p>}
      {restoreFileName && restoreFile.snapshot?.status === 'succeeded' && (
        <p className="text-xs text-grey-700">Saved and read back {restoreFileName}.</p>
      )}
      {erase.snapshot?.status === 'succeeded' && (
        <p className="text-xs text-grey-700">
          EEPROM 0x00-0x4C erased and checked
          {stepValue<{ file: string | null }>(erase.snapshot, 'delete-restore-file')?.file
            ? ', restore file deleted'
            : ''}
          . Power cycle the drive, then configure and calibrate the encoder.
        </p>
      )}

      {registers && (
        <div>
          <span className={labelCls}>Key registers</span>
          <table className="w-full text-xs">
            <thead>
              <tr className="text-left text-grey-500 border-b border-grey-200">
                <th className="py-1.5 pr-3 font-normal">RAM</th>
                <th className="py-1.5 pr-3 font-normal">EEPROM</th>
                <th className="py-1.5 pr-3 font-normal">Register</th>
                <th className="py-1.5 pr-3 font-normal">Value</th>
              </tr>
            </thead>
            <tbody>
              {registers.map(r => (
                <tr key={r.ramAddress} className="border-b border-grey-100 last:border-0">
                  <td className="py-1.5 pr-3 font-mono">{hex(r.ramAddress)}</td>
                  <td className="py-1.5 pr-3 font-mono">{hex(r.eepromAddress)}</td>
                  <td className="py-1.5 pr-3 font-mono">{r.register?.layout ?? 'not in the map'}</td>
                  <td className="py-1.5 pr-3 font-mono">
                    {hex(r.value)} ({r.value})
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}

      {exportBytes && (
        <div>
          <div className="flex items-center gap-3">
            <span className={labelCls}>EEPROM 0x00-0x4C</span>
            <button
              type="button"
              className="text-xs text-ocean underline"
              onClick={() =>
                downloadText(eepromCsv(exportBytes), `ic-mu-eeprom-encoder-${ordinal}.csv`, 'text/csv')
              }
            >
              Save CSV
            </button>
          </div>
          <div className="font-mono text-xs leading-5">
            {Array.from({ length: Math.ceil(exportBytes.length / 16) }, (_, row) => (
              <div key={row}>
                <span className="text-grey-500">{hex(row * 16)} </span>
                {exportBytes
                  .slice(row * 16, row * 16 + 16)
                  .map(b => b.value.toString(16).toUpperCase().padStart(2, '0'))
                  .join(' ')}
              </div>
            ))}
          </div>
        </div>
      )}
    </div>
  )
}
