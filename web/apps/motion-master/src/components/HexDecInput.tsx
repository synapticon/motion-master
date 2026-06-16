import { parseIntegerFlexible } from '@synapticon/motion-master-client'

// A text input for an integer value that can be viewed and edited in either decimal or
// hexadecimal. It is deliberately stateless: the "base" is read straight off the string
// (a leading 0x means hex), and the toggle simply rewrites the current value into the other
// base. The stored string stays in a form encodeSdoValue accepts directly — decimal, or
// signed-magnitude hex ("-0x1") for negatives — so a value round-trips through the encoder no
// matter which base it was last shown in. When `canHex` is false (floats, strings, raw bytes)
// it renders as a plain input with no toggle.

interface HexDecInputProps {
  value: string
  onChange: (value: string) => void
  // Whether the hex/dec toggle applies — true only for integer-typed objects.
  canHex?: boolean
  // Disables the text input (the value isn't editable, e.g. a read-only object). The hex/dec
  // toggle stays live so read-only values can still be viewed in either base; freeze it with
  // `toggleDisabled` (e.g. while an op is in flight).
  disabled?: boolean
  toggleDisabled?: boolean
  placeholder?: string
  title?: string
  // Visual classes for the <input> (borders/padding/font) — width is handled by the flex layout.
  inputClassName: string
  // Sizing classes for the wrapper (e.g. 'w-40' or 'w-full'); the input flexes to fill it.
  wrapperClassName?: string
  // Optional zero-padding for the hex form's digits (e.g. 4 → 0x000F), applied to the magnitude.
  hexPadDigits?: number
}

function isHexForm(value: string): boolean {
  return /^\s*-?0[xX]/.test(value)
}

// Reformat a flexible-int string into the requested base, or null if it isn't a parseable integer
// (empty, mid-edit, or a non-integer) — in which case the caller leaves the text untouched.
function toBase(value: string, base: 'dec' | 'hex', hexPadDigits?: number): string | null {
  const n = parseIntegerFlexible(value.trim())
  if (n === null) return null
  if (base === 'dec') return n.toString(10)
  const negative = n < 0n
  const magnitude = (negative ? -n : n).toString(16).toUpperCase()
  const padded = hexPadDigits ? magnitude.padStart(hexPadDigits, '0') : magnitude
  return `${negative ? '-' : ''}0x${padded}`
}

export default function HexDecInput({
  value,
  onChange,
  canHex = true,
  disabled = false,
  toggleDisabled = false,
  placeholder,
  title,
  inputClassName,
  wrapperClassName = 'inline-flex',
  hexPadDigits,
}: HexDecInputProps) {
  const hex = isHexForm(value)
  // The toggle can only fire when the current text is a parseable integer; mid-edit garbage or an
  // empty field leaves nothing to convert, so the button is inert rather than silently clearing.
  const converted = toBase(value, hex ? 'dec' : 'hex', hexPadDigits)
  const canToggle = canHex && !toggleDisabled && converted !== null

  return (
    <div className={`${wrapperClassName} items-stretch`}>
      <input
        type="text"
        value={value}
        onChange={e => onChange(e.target.value)}
        disabled={disabled}
        placeholder={placeholder}
        title={title}
        className={`${inputClassName} flex-1 min-w-0`}
      />
      {canHex && (
        <button
          type="button"
          onClick={() => converted !== null && onChange(converted)}
          disabled={!canToggle}
          className="inline-flex items-center justify-center border border-l-0 border-grey-300 px-2 text-[10px] font-display uppercase tracking-wide text-grey-500 hover:text-syn-red hover:border-grey-400 disabled:opacity-40 disabled:cursor-not-allowed cursor-pointer"
          title={
            hex
              ? 'Showing hexadecimal — click to switch to decimal'
              : 'Showing decimal — click to switch to hexadecimal'
          }
          aria-label={hex ? 'Switch to decimal' : 'Switch to hexadecimal'}
        >
          {hex ? 'dec' : 'hex'}
        </button>
      )}
    </div>
  )
}
