// The classes every row of DeviceBar shares, so its controls line up across rows.

export const labelCls = 'text-[10px] uppercase tracking-wide text-grey-500 font-display'

// The two buttons share one **explicit** height and are inline-flex centred, the same rule every
// control pair in this app follows. Left to derive their height from padding they do not agree:
// only the outline variant carries a border, so the pair rendered a few pixels apart — the shared
// btnPrimary/btnOutline are safe side by side only where nothing else varies.
//
// 30px, not the 38px used for form controls: this is always-on chrome above every device page, so
// its height is permanently spent, and a button here sits beside a badge rather than an input.
// A two-word label ("Set mode", "Quick stop", "Reset fault") would break across two lines inside a
// 30px box the moment the flex row squeezed it, so the label stays on one line and the box keeps
// its content width — the bar wraps whole controls onto a second row instead.
export const barBtn =
  'inline-flex shrink-0 items-center justify-center whitespace-nowrap h-[30px] px-4 text-xs ' +
  'cursor-pointer transition-colors disabled:opacity-50 disabled:cursor-not-allowed'
export const barBtnPrimary = `${barBtn} bg-syn-red text-white hover:bg-ocean`
export const barBtnOutline = `${barBtn} border border-syn-red text-syn-red hover:bg-syn-red hover:text-white`

// Selects share the buttons' explicit height for the reason in barBtn — a control pair that
// derives its height from content does not agree with one that does not.
export const barSelect =
  'h-[30px] border border-grey-300 bg-white px-2 text-xs disabled:opacity-50 ' +
  'disabled:cursor-not-allowed'
