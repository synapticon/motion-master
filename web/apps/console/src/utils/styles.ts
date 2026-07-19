// Shared outline button style (Synapticon-red border, fills on hover; dimmed + no-cursor when
// disabled). Used for the toolbar Refresh/action buttons so the look stays defined in one place.
export const btnOutline =
  'border border-syn-red text-syn-red px-3 py-1.5 text-xs hover:bg-syn-red hover:text-white disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

// Shared filled primary button (Synapticon-red fill, ocean on hover). The counterpart to
// btnOutline for the main action in a form/toolbar; same py-1.5 height so the two line up.
export const btnPrimary =
  'bg-syn-red text-white px-4 py-1.5 text-xs hover:bg-ocean disabled:opacity-50 ' +
  'disabled:cursor-not-allowed cursor-pointer transition-colors'
