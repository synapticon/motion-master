import { useMutation } from '@tanstack/react-query'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline } from '../utils/styles'

// Pulls the server's { error } message out of a thrown request, for inline display.
function apiError(err: unknown): string {
  if (err && typeof err === 'object' && 'error' in err) {
    const inner = (err as { error: unknown }).error
    if (inner && typeof inner === 'object' && 'error' in inner) {
      return String((inner as { error: unknown }).error)
    }
  }
  return 'Failed to write the dump.'
}

export default function RecorderPage() {
  const { api } = useConnection()

  const dumpMutation = useMutation({
    mutationFn: () => api.dumpProcessData(),
  })

  return (
    <div>
      <PageHeader
        eyebrow="Data"
        title="Recorder"
        description={
          <>
            The lossless process-data recorder captures every cyclic exchange into a circular ring
            held in memory — full raw inputs and outputs, an epoch-nanosecond timestamp, and the
            working counter for each cycle. It is the source for the live monitoring stream and for
            the dump below.
          </>
        }
      />
      <div className="p-4 sm:p-8 space-y-8">
        <div className="border border-grey-200 px-4 py-3 max-w-xl">
          <p className="eyebrow mb-1">Recorder dump</p>
          <p className="text-[11px] leading-snug text-grey-600">
            Writes every process-data cycle currently held in the recorder ring — full raw inputs
            and outputs, with the current process-image layout embedded as a header — to a{' '}
            <span className="font-mono">.mmpd</span> file for offline analysis. Captures the ring
            from oldest to newest at the moment you click; works while exchanging too. The file is
            written on the machine running Motion Master (default: its temp directory) — the path
            below is local to that machine.
          </p>
          <button
            onClick={() => dumpMutation.mutate()}
            disabled={dumpMutation.isPending}
            className={`${btnOutline} mt-3`}
          >
            {dumpMutation.isPending ? 'Dumping…' : 'Dump recorder ring'}
          </button>
          {dumpMutation.isSuccess && (
            <p className="text-[11px] text-status-good mt-2 break-all">
              Wrote <span className="font-mono">{dumpMutation.data.data.path}</span>
            </p>
          )}
          {dumpMutation.isError && (
            <p className="text-[11px] text-status-bad mt-2">{apiError(dumpMutation.error)}</p>
          )}
        </div>
      </div>
    </div>
  )
}
