import type { FsoeConnection } from '@synapticon/motion-master-client'

/**
 * The live safe values the drive publishes, and the raw octets they were decoded
 * from.
 *
 * A body rather than a section of its own: these numbers and the sensor
 * configuration below them are one subject - the safe measuring channel - and
 * splitting them made the reader carry a value in their head to compare it with
 * the tolerance that judges it. @c SafeMeasurementsPanel is the container.
 *
 * The octets stay with the values rather than with the connection detail: they
 * are what these numbers ARE, and reading them side by side is how somebody
 * checks a decode they do not yet trust.
 */

function hex(n: number, digits = 2): string {
  return `0x${n.toString(16).toUpperCase().padStart(digits, '0')}`
}

function octets(bytes: number[] | undefined): string {
  return (bytes ?? []).map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ')
}

export function Badge({ ok, label, title }: { ok: boolean; label: string; title?: string }) {
  return (
    <span
      title={title}
      className={`inline-flex items-center h-[18px] px-1.5 text-[10px] tracking-wide ${
        ok ? 'bg-status-good text-white' : 'bg-grey-200 text-grey-600'
      } ${title ? 'cursor-help' : ''}`}
    >
      {label}
    </span>
  )
}

/**
 * A safe value with its own validity. The number is greyed when the drive says it is not usable,
 * because a drive encodes an invalid channel as zero rather than holding its last good reading —
 * so a plain "0" beside a cleared flag would read as a real measurement.
 */
function SafeValue({
  label,
  value,
  unit,
  valid,
  extra,
}: {
  label: string
  value: string
  unit: string
  valid: boolean
  extra?: React.ReactNode
}) {
  return (
    <div className="border border-grey-200 px-4 py-3">
      <div className="flex items-center justify-between gap-2">
        <span className="text-[10px] uppercase tracking-wider text-grey-500">{label}</span>
        <Badge ok={valid} label={valid ? 'valid' : 'invalid'} />
      </div>
      <div className={`mt-1 font-mono text-xl ${valid ? 'text-grey-900' : 'text-grey-400'}`}>
        {value}
        <span className="ml-1 text-xs text-grey-500">{unit}</span>
      </div>
      {extra}
    </div>
  )
}

export default function SafeProcessValuesBody({ connection }: { connection: FsoeConnection }) {
  const values = connection.processValues

  return (
    <>
      <div className="p-4 grid gap-4 sm:grid-cols-3">
        <SafeValue
          label="Safe position"
          value={(values?.positionRevolutions ?? 0).toFixed(4)}
          unit="rev"
          valid={(values?.positionValid ?? false) && connection.inputsValid}
          extra={
            <div className="mt-1 font-mono text-[11px] text-grey-500">
              {hex(values?.position ?? 0, 8)} · 8.24 fixed point
            </div>
          }
        />
        <SafeValue
          label="Safe velocity"
          value={(values?.velocityMilliRpm ?? 0).toLocaleString()}
          unit="mrpm"
          valid={(values?.velocityValid ?? false) && connection.inputsValid}
        />
        <SafeValue
          label="Safe torque"
          value={(values?.torqueMillinewtonMetres ?? 0).toLocaleString()}
          unit="mNm"
          valid={(values?.torqueValid ?? false) && connection.inputsValid}
        />
      </div>
      <div className="px-4 pb-4 grid gap-4 sm:grid-cols-2">
        <div>
          <dt className="text-[10px] uppercase tracking-wider text-grey-500">SafeInputs</dt>
          <dd
            title="The SafeData octets received, after the CRC and sequence checks passed. Octet 0 is the safety statusword, octet 1 the validity bits."
            className="font-mono text-xs cursor-help"
          >
            {octets(connection.safeInputs)}
          </dd>
        </div>
        <div>
          <dt className="text-[10px] uppercase tracking-wider text-grey-500">SafeOutputs</dt>
          <dd
            title="The SafeData octets being sent. Octet 0 is the safety controlword."
            className="font-mono text-xs cursor-help"
          >
            {octets(connection.safeOutputs)}
          </dd>
        </div>
      </div>
    </>
  )
}
