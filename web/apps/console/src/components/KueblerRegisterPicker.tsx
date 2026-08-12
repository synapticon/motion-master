import { useQuery } from '@tanstack/react-query'
import { useConnection } from '../contexts/ConnectionContext'

// Matches the shared control height used by every form control on the device pages: this theme's
// spacing scale is geometric (h-9 = 6rem = 96px!), so it has to be explicit px.
const selectCls = 'border border-grey-300 px-3 h-[38px] text-sm w-full bg-white'

/** How the encoder's own draft says a register's assembled value should be read. */
const FORMAT_LABEL: Record<string, string> = {
  unsigned: 'unsigned',
  signed: 'signed',
  bitField: 'bit field',
  signedHalves: 'two signed 16-bit halves',
}

function hex2(value: number): string {
  return `0x${value.toString(16).toUpperCase().padStart(2, '0')}`
}

/**
 * A picker over the Integro internal encoder's register map, which fills the address and length of
 * the `kuebler-register-communication` procedure's form.
 *
 * **It sits beside the generic parameter form rather than inside it.** The procedure's parameters are
 * a plain address and length — the raw mechanism, and the only thing that can address a register the
 * vendor's draft does not document — so this is a convenience over them, not a replacement. That
 * also keeps ProcedureParameters free of any one procedure's knowledge: this component's only
 * contract with it is the two `onChange` calls.
 *
 * The map is static per encoder type rather than per device, so it is fetched once and kept.
 */
export default function KueblerRegisterPicker({
  disabled,
  onPick,
}: {
  disabled: boolean
  /** Called with the chosen register's address and its width in bytes. */
  onPick: (address: number, lengthBytes: number) => void
}) {
  const { api } = useConnection()

  const registersQuery = useQuery({
    queryKey: ['kueblerRegisters'],
    queryFn: () => api.getKueblerRegisters(),
    staleTime: Infinity,
  })
  const registers = registersQuery.data?.data ?? []

  if (registersQuery.isError) {
    // Not a failure of the procedure: its address and length fields still work without the map.
    return null
  }

  return (
    <div className="space-y-1">
      <label className="text-[10px] uppercase tracking-wide text-grey-500 font-display block">
        Pick a register
      </label>
      <select
        className={selectCls}
        disabled={disabled || registers.length === 0}
        defaultValue=""
        onChange={(e) => {
          const chosen = registers.find((r) => String(r.address) === e.target.value)
          if (chosen) {
            onPick(chosen.address, chosen.bits / 8)
          }
        }}
      >
        <option value="">
          {registers.length === 0 ? 'Loading the register map…' : 'Choose a register…'}
        </option>
        {registers.map((r) => {
          // Both reasons a register cannot simply be read are marked rather than hidden: one is too
          // wide for the command, and seven are documented but absent from the encoder.
          const notes = [
            !r.readableInOneCommand ? `${r.bits}-bit — too wide for one command` : '',
            !r.implemented ? 'not implemented' : '',
            r.access !== 'rw' ? r.access.toUpperCase() : '',
            FORMAT_LABEL[r.format] ?? r.format,
          ].filter(Boolean)
          return (
            <option key={r.address} value={r.address} disabled={!r.readableInOneCommand}>
              {hex2(r.address)} — {r.name} ({notes.join(', ')})
            </option>
          )
        })}
      </select>
      <p className="text-xs text-grey-600">
        Fills the address and length below from the encoder's register map. The fields stay editable,
        which is the only way to reach an address the vendor's draft does not document.
      </p>
    </div>
  )
}
