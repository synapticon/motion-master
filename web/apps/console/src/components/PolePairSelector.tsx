// The pole-pair control, shared by every figure on a Learn page that depends on it.
//
// One control per figure, all driving the same state, so the reader can change it wherever they
// happen to be reading rather than scrolling back to a single master control.

export default function PolePairSelector({
  value,
  onChange,
  hint,
}: {
  value: number
  onChange: (polePairs: number) => void
  hint?: string
}) {
  return (
    <div>
      <span className="block text-xs text-grey-700 mb-1.5">Pole pairs</span>
      <div className="flex gap-1">
        {[1, 2, 3, 4].map(p => (
          <button
            key={p}
            type="button"
            aria-pressed={value === p}
            onClick={() => onChange(p)}
            className={`h-[38px] w-12 inline-flex items-center justify-center text-xs border transition-colors cursor-pointer ${
              value === p
                ? 'border-syn-red bg-syn-red text-white'
                : 'border-grey-300 text-grey-700 hover:border-syn-red hover:text-syn-red'
            }`}
          >
            {p}
          </button>
        ))}
      </div>
      {hint && <p className="text-[11px] text-grey-500 leading-4 mt-1">{hint}</p>}
    </div>
  )
}
