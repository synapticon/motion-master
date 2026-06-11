import { useState } from 'react'
import type { ReactNode } from 'react'

const hex2 = (b: number) => b.toString(16).padStart(2, '0').toUpperCase()
const asciiChar = (b: number) => (b >= 32 && b < 127 ? String.fromCharCode(b) : '.')
const HL = 'bg-syn-red text-white'

interface HexViewerProps {
  /** Bytes to render. Callers that want to cap the view should slice before passing. */
  bytes: Uint8Array
  /** Width of the offset column in hex digits (default 4). */
  offsetDigits?: number
}

// An offset / hex / ASCII dump where hovering a byte cross-highlights it in both the hex and ASCII
// columns (and vice-versa) — the standard hex-editor affordance for lining a value up with its
// character. Rendered as per-byte <span>s (not a single string) so each cell can react to hover.
// Shared across pages that display raw bytes (SII EEPROM, FoE file reads, …).
export default function HexViewer({ bytes, offsetDigits = 4 }: HexViewerProps) {
  const [hovered, setHovered] = useState<number | null>(null)
  const rows: number[] = []
  for (let i = 0; i < bytes.length; i += 16) {
    rows.push(i)
  }
  return (
    <div
      onMouseLeave={() => setHovered(null)}
      className="border border-grey-200 bg-grey-50 p-4 text-[11px] leading-relaxed font-mono overflow-x-auto whitespace-pre"
    >
      {rows.map(rowStart => {
        const cells: ReactNode[] = [
          <span key="off" className="text-grey-400">
            {rowStart.toString(16).padStart(offsetDigits, '0').toUpperCase()}
          </span>,
          '  ',
        ]
        // Hex column: 16 two-digit cells, an extra gap after the 8th, blanks past the end.
        for (let c = 0; c < 16; c++) {
          const idx = rowStart + c
          if (c === 8) {
            cells.push(' ')
          }
          if (idx < bytes.length) {
            cells.push(
              <span
                key={`h${c}`}
                onMouseEnter={() => setHovered(idx)}
                className={`cursor-pointer ${hovered === idx ? HL : ''}`}
              >
                {hex2(bytes[idx])}
              </span>,
            )
          } else {
            cells.push('  ')
          }
          cells.push(' ')
        }
        // ASCII column.
        cells.push(' |')
        for (let c = 0; c < 16; c++) {
          const idx = rowStart + c
          if (idx < bytes.length) {
            cells.push(
              <span
                key={`a${c}`}
                onMouseEnter={() => setHovered(idx)}
                className={`cursor-pointer ${hovered === idx ? HL : ''}`}
              >
                {asciiChar(bytes[idx])}
              </span>,
            )
          } else {
            cells.push(' ')
          }
        }
        cells.push('|')
        return <div key={rowStart}>{cells}</div>
      })}
    </div>
  )
}
