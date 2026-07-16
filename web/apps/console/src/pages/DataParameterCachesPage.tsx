import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { ParameterCacheEntry } from '@synapticon/motion-master-client'
import PageHeader from '../components/PageHeader'
import ParameterCacheExplainer from '../components/ParameterCacheExplainer'
import { useConnection } from '../contexts/ConnectionContext'
import { downloadBytes } from '../utils/download'
import { btnOutline } from '../utils/styles'

const hex = (n: number) => `0x${n.toString(16).toUpperCase()}`

const formatSize = (bytes: number) =>
  bytes < 1024 ? `${bytes} B` : `${(bytes / 1024).toFixed(1)} KB`

export default function DataParameterCachesPage() {
  const { api } = useConnection()
  const queryClient = useQueryClient()
  const [downloadingId, setDownloadingId] = useState<string | null>(null)

  // Caches live on disk, independent of the bus — no scan/connect gate. The query simply fails if
  // Motion Master is unreachable, surfaced as the error state below.
  const cachesQuery = useQuery({
    queryKey: ['parameter-caches'],
    queryFn: () => api.listParameterCaches(),
  })
  const caches = cachesQuery.data?.data ?? []

  const deleteMutation = useMutation({
    mutationFn: (id: string) => api.deleteParameterCache(id),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['parameter-caches'] }),
  })

  async function handleDownload(entry: ParameterCacheEntry) {
    setDownloadingId(entry.id)
    try {
      const res = await fetch(`${api.baseUrl}/api/parameter-caches/${entry.id}`)
      if (!res.ok) {
        throw new Error(`HTTP ${res.status}`)
      }
      const bytes = new Uint8Array(await res.arrayBuffer())
      downloadBytes(bytes, `parameters-${entry.id}.json`)
    } finally {
      setDownloadingId(null)
    }
  }

  function handleDelete(entry: ParameterCacheEntry) {
    const confirmed = window.confirm(
      `Delete the cached object dictionary for ${hex(entry.vendorId)} / ${hex(entry.productCode)} / ` +
        `${hex(entry.revisionNumber)}?\n\nThis only removes Motion Master's saved copy — the device ` +
        `is untouched. The next scan of this hardware will re-enumerate it over SDO.`,
    )
    if (confirmed) {
      deleteMutation.mutate(entry.id)
    }
  }

  function handleClearAll() {
    if (caches.length === 0) {
      return
    }
    const confirmed = window.confirm(
      `Delete all ${caches.length} cached object ${caches.length === 1 ? 'dictionary' : 'dictionaries'}?\n\n` +
        `Devices are untouched; the next scan re-enumerates them over SDO.`,
    )
    if (confirmed) {
      caches.forEach(entry => deleteMutation.mutate(entry.id))
    }
  }

  return (
    <div>
      <PageHeader
        eyebrow="Data"
        title="Parameter Caches"
        description="Manage Motion Master's on-disk cache of device object dictionaries. Download a cached definition to inspect it offline, or delete one to force a fresh enumeration on the next scan."
      />
      <div className="p-4 sm:px-8 sm:py-7 space-y-6">
        <ParameterCacheExplainer />

        <div className="flex items-center justify-between gap-3">
          <p className="eyebrow text-xs">
            {caches.length} cached {caches.length === 1 ? 'dictionary' : 'dictionaries'}
          </p>
          <div className="flex items-center gap-3">
            <button
              onClick={handleClearAll}
              disabled={caches.length === 0 || deleteMutation.isPending}
              className={btnOutline}
            >
              Clear all
            </button>
            <button
              onClick={() => cachesQuery.refetch()}
              disabled={cachesQuery.isFetching}
              className={btnOutline}
            >
              {cachesQuery.isFetching ? 'Loading…' : 'Refresh'}
            </button>
          </div>
        </div>

        {cachesQuery.isError && (
          <p className="text-xs text-status-bad font-mono">
            Failed to read parameter caches. Is Motion Master running?
          </p>
        )}

        {!cachesQuery.isError && caches.length === 0 && (
          <p className="text-sm text-grey-500">
            Nothing cached yet. Scan a bus with Synapticon drives (or enable caching for other
            vendors in the config) and their object dictionaries will be cached here.
          </p>
        )}

        {caches.length > 0 && (
          <div className="border border-grey-200 overflow-x-auto">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  {['Vendor ID', 'Product Code', 'Revision', 'Parameters', 'Size', ''].map(h => (
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
                {caches.map(entry => (
                  <tr key={entry.id} className="border-b border-grey-100 last:border-0">
                    <td className="px-4 py-2 font-mono">{hex(entry.vendorId)}</td>
                    <td className="px-4 py-2 font-mono">{hex(entry.productCode)}</td>
                    <td className="px-4 py-2 font-mono">{hex(entry.revisionNumber)}</td>
                    <td className="px-4 py-2">{entry.parameterCount}</td>
                    <td className="px-4 py-2">{formatSize(entry.sizeBytes)}</td>
                    <td className="px-4 py-2">
                      <div className="flex items-center justify-end gap-2">
                        <button
                          onClick={() => handleDownload(entry)}
                          disabled={downloadingId === entry.id}
                          className={btnOutline}
                        >
                          {downloadingId === entry.id ? 'Downloading…' : 'Download'}
                        </button>
                        <button
                          onClick={() => handleDelete(entry)}
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
