// One shared control height for every input and button on the device pages: this theme's spacing
// scale is geometric, so a numeric height utility would not land on ~38px.
//
// Shared rather than redeclared per file so a panel extracted out of a page keeps the page's
// controls, which is the whole point of extracting it.
export const inputCls = 'border border-grey-300 px-3 h-[38px] text-sm w-full bg-white font-mono'

/** The affirmative control: the one that makes something happen. */
export const btnCls =
  'inline-flex shrink-0 items-center justify-center whitespace-nowrap bg-syn-red text-white px-4 ' +
  'h-[38px] text-xs hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed ' +
  'cursor-pointer transition-colors'

/** The reversible or secondary control. */
export const btnGhostCls =
  'inline-flex shrink-0 items-center justify-center whitespace-nowrap border border-grey-300 ' +
  'text-grey-700 px-4 h-[38px] text-xs hover:bg-grey-50 disabled:opacity-50 ' +
  'disabled:cursor-not-allowed cursor-pointer transition-colors'
