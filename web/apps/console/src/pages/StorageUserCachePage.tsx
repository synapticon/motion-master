import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { UserCacheFile } from '@synapticon/motion-master-client'
import FilePickerButton from '../components/FilePickerButton'
import PageHeader from '../components/PageHeader'
import UserCacheExplainer from '../components/UserCacheExplainer'
import { WireTiming, useWireTiming } from '../components/WireTiming'
import { useConnection } from '../contexts/ConnectionContext'
import { downloadBytes } from '../utils/download'
import { formatBytes } from '../utils/format'
import { encodeUserCachePath, userCacheBasename, userCacheUrl } from '../utils/userCache'
import { btnOutline } from '../utils/styles'

// The `X-Wire-Us` figure on these endpoints is server-side file I/O, not a fieldbus transaction —
// WireTiming's default tooltip would claim a device was involved, so override it.
const SERVER_TIMING_TITLE =
  'Server — time the backend spent on the operation itself: validating the path and reading, ' +
  'writing or listing on the server’s own disk. No device is involved. The round-trip figure ' +
  'beside it additionally covers transferring the file over the network, which for a large ' +
  'upload or download is most of the wait.'

const formatModified = (ms: number) => (ms > 0 ? new Date(ms).toLocaleString() : '—')

// Normalises what the user typed into a destination folder: trims, drops leading/trailing and
// doubled slashes. The backend rejects all of those outright, and silently tidying a stray slash
// is friendlier than a 400 on a path the user clearly meant.
function normalizeFolder(folder: string): string {
  return folder
    .split('/')
    .map(segment => segment.trim())
    .filter(Boolean)
    .join('/')
}

export default function StorageUserCachePage() {
  const { api } = useConnection()
  const queryClient = useQueryClient()
  const [folder, setFolder] = useState('')
  const [busyPath, setBusyPath] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)

  // Two readouts, each next to the control that produces it: uploads by the Upload button, and
  // everything that acts on the listing (refresh, download, delete) by the Refresh button.
  const uploadTiming = useWireTiming()
  const fileTiming = useWireTiming()

  // The cache is on disk, independent of the bus — no scan/connect gate. The query fails only if
  // Motion Master itself is unreachable, surfaced as the error state below.
  const cacheQuery = useQuery({
    queryKey: ['user-cache'],
    queryFn: () => fileTiming.measure(() => api.listUserCacheFiles()),
  })
  const root = cacheQuery.data?.data?.root ?? ''
  const files = cacheQuery.data?.data?.files ?? []
  const totalBytes = files.reduce((sum, file) => sum + file.size, 0)

  const invalidate = () => queryClient.invalidateQueries({ queryKey: ['user-cache'] })

  const uploadMutation = useMutation({
    mutationFn: async (file: File) => {
      const prefix = normalizeFolder(folder)
      const path = prefix ? `${prefix}/${file.name}` : file.name
      // The generated client JSON-encodes request bodies, so upload the raw bytes with fetch —
      // the same reason the FoE and SII pages talk to their binary endpoints directly.
      const res = await uploadTiming.measure(() =>
        fetch(userCacheUrl(api.baseUrl, path), {
          method: 'PUT',
          headers: { 'Content-Type': 'application/octet-stream' },
          body: file,
        }),
      )
      if (!res.ok) {
        const body = await res.json().catch(() => null)
        throw new Error(body?.error ?? `HTTP ${res.status}`)
      }
    },
    onSuccess: () => {
      setError(null)
      invalidate()
    },
    onError: (err: Error) => setError(err.message),
  })

  const deleteMutation = useMutation({
    mutationFn: (path: string) =>
      fileTiming.measure(() => api.deleteUserCacheFile(encodeUserCachePath(path))),
    onSuccess: () => {
      setError(null)
      invalidate()
    },
    onError: (err: Error) => setError(err.message),
  })

  async function handleDownload(file: UserCacheFile) {
    setBusyPath(file.path)
    setError(null)
    try {
      const res = await fileTiming.measure(() => fetch(userCacheUrl(api.baseUrl, file.path)))
      if (!res.ok) {
        throw new Error(`HTTP ${res.status}`)
      }
      const bytes = new Uint8Array(await res.arrayBuffer())
      // Save under the file's own name, not its full path — a browser download cannot create
      // directories, and a slash in the suggested name is stripped anyway.
      downloadBytes(bytes, userCacheBasename(file.path))
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Download failed.')
    } finally {
      setBusyPath(null)
    }
  }

  function handleDelete(file: UserCacheFile) {
    const confirmed = window.confirm(
      `Delete ${file.path} from the user cache?\n\nThis removes the file from this machine's ` +
        `disk. No device is touched.`,
    )
    if (confirmed) {
      deleteMutation.mutate(file.path)
    }
  }

  const destination = normalizeFolder(folder)

  return (
    <div>
      <PageHeader
        eyebrow="Storage"
        title="User Cache"
        description="Store files on the machine running Motion Master so they survive a restart. Uploading does not by itself make Motion Master do anything with a file — but some folders here belong to features that read their own."
      />
      <div className="p-4 sm:px-8 sm:py-7 space-y-6">
        <UserCacheExplainer />

        {/* The destination reads as one path: a fixed, non-editable prefix showing where the store
            actually is on the server, then the folder you type. Composing them in a single bordered
            group is what makes "uploads land here" legible without a separate hint line. */}
        <div className="flex flex-wrap items-center gap-3">
          <label className="text-xs text-grey-600 uppercase tracking-wide" htmlFor="folder">
            Folder
          </label>
          <div className="flex items-stretch h-[38px] border border-grey-300 bg-white">
            {root && (
              <span
                title={`Cache directory on the server — ${root}`}
                className="flex items-center shrink-0 px-3 bg-grey-50 border-r border-grey-300 font-mono text-xs text-grey-500 cursor-help"
              >
                {root}/
              </span>
            )}
            <input
              id="folder"
              type="text"
              value={folder}
              onChange={e => setFolder(e.target.value)}
              placeholder="optional subfolder"
              className="w-48 min-w-0 px-3 text-sm bg-transparent focus:outline-none"
            />
          </div>
          <FilePickerButton
            onFile={file => uploadMutation.mutate(file)}
            accept="*"
            className="h-[38px]"
            disabled={uploadMutation.isPending}
            title={`Upload a file into ${destination ? `${destination}/` : 'the cache root'}`}
          >
            {uploadMutation.isPending ? 'Uploading…' : 'Upload file'}
          </FilePickerButton>
          <WireTiming label="Server" timing={uploadTiming.timing} title={SERVER_TIMING_TITLE} />
        </div>

        {/* The timing grows out of the left-hand group rather than sitting in front of Refresh, so
            the button stays pinned to the right edge and never jumps as a readout appears. */}
        <div className="flex items-center justify-between gap-3">
          <div className="flex items-center gap-3 min-w-0">
            <p className="eyebrow text-xs">
              {files.length} {files.length === 1 ? 'file' : 'files'}
              {files.length > 0 && ` · ${formatBytes(totalBytes)}`}
            </p>
            <WireTiming label="Server" timing={fileTiming.timing} title={SERVER_TIMING_TITLE} />
          </div>
          <button
            onClick={() => cacheQuery.refetch()}
            disabled={cacheQuery.isFetching}
            className={btnOutline}
          >
            {cacheQuery.isFetching ? 'Loading…' : 'Refresh'}
          </button>
        </div>

        {error && <p className="text-xs text-status-bad font-mono">{error}</p>}

        {cacheQuery.isError && (
          <p className="text-xs text-status-bad font-mono">
            Failed to read the user cache. Is Motion Master running?
          </p>
        )}

        {!cacheQuery.isError && files.length === 0 && (
          <p className="text-sm text-grey-500">
            The cache is empty. Upload a file to keep it on the server between restarts.
          </p>
        )}

        {files.length > 0 && (
          <div className="border border-grey-200 overflow-x-auto">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  {['Path', 'Size', 'Modified', ''].map(h => (
                    <th
                      key={h}
                      className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium"
                    >
                      {h}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {files.map(file => (
                  <tr key={file.path} className="border-b border-grey-100 last:border-0">
                    <td className="px-4 py-2 font-mono break-all">{file.path}</td>
                    <td className="px-4 py-2 whitespace-nowrap">{formatBytes(file.size)}</td>
                    <td className="px-4 py-2 whitespace-nowrap">
                      {formatModified(file.modifiedMs)}
                    </td>
                    <td className="px-4 py-2">
                      <div className="flex items-center justify-end gap-2">
                        <button
                          onClick={() => handleDownload(file)}
                          disabled={busyPath === file.path}
                          className={btnOutline}
                        >
                          {busyPath === file.path ? 'Downloading…' : 'Download'}
                        </button>
                        <button
                          onClick={() => handleDelete(file)}
                          disabled={deleteMutation.isPending}
                          className="border border-grey-300 text-grey-700 px-3 py-1.5 text-xs hover:bg-grey-50 disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors"
                        >
                          Delete
                        </button>
                      </div>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </div>
    </div>
  )
}
