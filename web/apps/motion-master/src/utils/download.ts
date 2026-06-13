// Triggers a browser download of raw bytes by creating a transient object URL
// and synthetic anchor. This is exactly what the file-saver library does for
// modern browsers, without the legacy-browser baggage or extra dependency.
export function downloadBytes(bytes: Uint8Array, filename: string) {
  const blob = new Blob([bytes as BlobPart], { type: 'application/octet-stream' })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = filename || 'file.bin'
  document.body.appendChild(a)
  a.click()
  a.remove()
  URL.revokeObjectURL(url)
}
