// Helpers for SOMANET (Synapticon) drive specifics.

export interface SomanetFile {
  name: string
  size: number | null
}

// Parse the body of a SOMANET `fs-getlist` FoE read. Each entry is one line in
// the form `<filename>, size: <bytes>`; size is null when a line doesn't match.
export function parseSomanetFileList(text: string): SomanetFile[] {
  return text
    .split(/\r?\n/)
    .map(line => line.trim())
    .filter(Boolean)
    .map(line => {
      const match = line.match(/^(.*?),\s*size:\s*(\d+)\s*$/)
      if (match) return { name: match[1].trim(), size: Number(match[2]) }
      return { name: line, size: null }
    })
}
