import type { ProcedureParameter } from '@synapticon/motion-master-client'

// One shared control height for every interactive control, matching the device pages: this theme's
// spacing scale is geometric (h-9 = 6rem = 96px!), so the height has to be explicit px.
const inputCls = 'border border-grey-300 px-3 h-[38px] text-sm w-full bg-white'
const labelCls = 'text-[10px] uppercase tracking-wide text-grey-500 font-display block'

/**
 * What the fields currently hold. Strings and booleans rather than parsed values, because a
 * half-typed number is a legitimate state of a text field and has no number to be.
 */
export type ParameterValues = Record<string, string | boolean>

/** The fields as a procedure's descriptor says they should start: every default, filled in. */
export function initialParameterValues(parameters: ProcedureParameter[]): ParameterValues {
  const values: ParameterValues = {}
  for (const parameter of parameters) {
    if (parameter.type === 'boolean') {
      values[parameter.name] = parameter.defaultValue === true
    } else if (parameter.defaultValue === undefined || parameter.defaultValue === null) {
      values[parameter.name] = ''
    } else {
      values[parameter.name] = String(parameter.defaultValue)
    }
  }
  return values
}

/** A decimal or `0x`-prefixed hex integer, or null when the text is not one. */
function parseInteger(text: string): number | null {
  const token = text.trim()
  if (token === '') {
    return null
  }
  const value = /^0[xX][0-9a-fA-F]+$/.test(token) ? Number.parseInt(token.slice(2), 16) : Number(token)
  return Number.isInteger(value) ? value : null
}

/**
 * Turns the fields into a request body, or says what is wrong with them.
 *
 * The same checks the server applies, done here only so a mistake is caught before a request goes
 * out — the server validates whatever arrives regardless. `body` is null while anything is wrong,
 * which is what disables Run; `errors` is keyed by parameter name and is empty when it is not.
 *
 * An empty field is reported as `missing` rather than as an error message: a required field nobody
 * has typed in yet should block Run without shouting at someone who has just opened the page.
 */
export function parseParameterValues(
  parameters: ProcedureParameter[],
  values: ParameterValues,
): { body: Record<string, unknown> | null; errors: Record<string, string>; missing: boolean } {
  const body: Record<string, unknown> = {}
  const errors: Record<string, string> = {}
  let missing = false

  for (const parameter of parameters) {
    const raw = values[parameter.name]

    if (parameter.type === 'boolean') {
      body[parameter.name] = raw === true
      continue
    }

    const text = typeof raw === 'string' ? raw.trim() : ''
    if (text === '') {
      // Only a required field blocks: an optional one left empty means "use the default", which is
      // exactly what omitting it from the body does.
      if (parameter.required) {
        missing = true
      }
      continue
    }

    if (parameter.type === 'byteArray') {
      const length = parameter.length ?? 0
      const tokens = text.split(/[\s,]+/).filter(t => t.length > 0)
      if (tokens.length !== length) {
        errors[parameter.name] = `Enter exactly ${length} bytes (got ${tokens.length}).`
        continue
      }
      const bytes: number[] = []
      for (const token of tokens) {
        const value = parseInteger(token)
        if (value === null || value < 0 || value > 255) {
          errors[parameter.name] = `"${token}" is not a byte value (0-255, or 0x00-0xFF).`
          break
        }
        bytes.push(value)
      }
      if (bytes.length === length) {
        body[parameter.name] = bytes
      }
      continue
    }

    if (parameter.type === 'enum') {
      // The option's own value goes on the wire, not the string the select carried it as — a
      // parameter whose options are numbers must not turn into strings on the way out.
      const option = (parameter.options ?? []).find(o => String(o.value) === text)
      if (option === undefined) {
        errors[parameter.name] = 'Pick one of the listed values.'
        continue
      }
      body[parameter.name] = option.value
      continue
    }

    const value = parseInteger(text)
    if (value === null) {
      errors[parameter.name] = 'Enter a whole number, decimal or 0x hex.'
      continue
    }
    const min = parameter.minValue
    const max = parameter.maxValue
    if ((min !== undefined && value < min) || (max !== undefined && value > max)) {
      errors[parameter.name] = `Enter a value between ${min} and ${max}.`
      continue
    }
    body[parameter.name] = value
  }

  const ok = !missing && Object.keys(errors).length === 0
  return { body: ok ? body : null, errors, missing }
}

/**
 * The fields for one procedure's parameters, rendered from what its descriptor declares.
 *
 * Generic on purpose: the console knows the four parameter types, not the procedures. Adding a
 * procedure that takes parameters is a row in the server's catalogue and nothing here.
 */
export default function ProcedureParameters({
  parameters,
  values,
  errors,
  disabled,
  onChange,
}: {
  parameters: ProcedureParameter[]
  values: ParameterValues
  errors: Record<string, string>
  disabled: boolean
  onChange: (name: string, value: string | boolean) => void
}) {
  if (parameters.length === 0) {
    return null
  }
  return (
    <div className="space-y-4 max-w-3xl">
      {parameters.map(parameter => {
        const id = `parameter-${parameter.name}`
        const value = values[parameter.name]
        const error = errors[parameter.name]
        return (
          <div key={parameter.name} className="space-y-1">
            {/* A checkbox names itself, so it carries the title beside it instead of above it —
                repeating it in both places would read as two separate things. The row keeps the
                shared control height either way, so the fields stay aligned. */}
            {parameter.type !== 'boolean' && (
              <label htmlFor={id} className={labelCls}>
                {parameter.title}
                {parameter.required && <span className="text-syn-red"> *</span>}
              </label>
            )}

            {parameter.type === 'boolean' ? (
              <label htmlFor={id} className="flex items-center gap-2 h-[38px] cursor-pointer">
                <input
                  id={id}
                  type="checkbox"
                  className="accent-syn-red"
                  checked={value === true}
                  onChange={e => onChange(parameter.name, e.target.checked)}
                  disabled={disabled}
                />
                <span className={labelCls}>{parameter.title}</span>
              </label>
            ) : parameter.type === 'enum' ? (
              <select
                id={id}
                className={inputCls}
                value={typeof value === 'string' ? value : ''}
                onChange={e => onChange(parameter.name, e.target.value)}
                disabled={disabled}
              >
                {/* A required enum starts with nothing chosen, and a select cannot show "nothing" —
                    left to itself it would display the first option while holding no value, so the
                    user would see a choice they never made and a Run button disabled for no visible
                    reason. The placeholder is that empty state, made visible. */}
                {parameter.required && (
                  <option value="" disabled>
                    Select…
                  </option>
                )}
                {(parameter.options ?? []).map(option => (
                  <option key={String(option.value)} value={String(option.value)}>
                    {option.title}
                  </option>
                ))}
              </select>
            ) : (
              <input
                id={id}
                className={inputCls}
                // Text rather than number for every integer, so a register address can be typed the
                // way its documentation writes it — 0x75 — and the bounds still come from the
                // descriptor rather than from the browser.
                type="text"
                inputMode={parameter.type === 'integer' ? 'numeric' : 'text'}
                placeholder={parameter.type === 'byteArray' ? '13 0 0 0 0 0 0 0' : ''}
                value={typeof value === 'string' ? value : ''}
                onChange={e => onChange(parameter.name, e.target.value)}
                disabled={disabled}
              />
            )}

            <p className="text-xs text-grey-500">
              {parameter.description}
              {parameter.type === 'integer' &&
                parameter.minValue !== undefined &&
                parameter.maxValue !== undefined && (
                  <>
                    {' '}
                    <span className="text-grey-400">
                      {parameter.minValue}–{parameter.maxValue}, decimal or 0x hex.
                    </span>
                  </>
                )}
              {parameter.type === 'byteArray' && (
                <>
                  {' '}
                  <span className="text-grey-400">
                    {parameter.length} bytes, decimal or 0x hex, separated by spaces or commas.
                  </span>
                </>
              )}
            </p>
            {error && <p className="text-status-bad text-xs">{error}</p>}
          </div>
        )
      })}
    </div>
  )
}
