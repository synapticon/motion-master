const labelCls = 'text-[10px] uppercase tracking-wide text-grey-500 font-display block'

/**
 * One labelled row of a decoded value.
 *
 * `hint` says what the field is for rather than restating it — these names come from specifications
 * most people have not read.
 */
export default function FieldRow({
  label,
  value,
  hint,
}: {
  label: string
  value: string
  hint?: string
}) {
  return (
    <div className="grid grid-cols-[11rem_1fr] gap-3 py-1.5 border-b border-grey-100 last:border-0">
      <div className={labelCls}>{label}</div>
      <div className="text-sm">
        <span className="font-mono break-all">{value}</span>
        {hint && <span className="text-grey-400 text-xs"> — {hint}</span>}
      </div>
    </div>
  )
}
