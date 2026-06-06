import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline } from '../utils/styles'

const inputCls = 'border border-grey-300 px-3 py-2 text-sm w-full bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'

function apiError(err: unknown): string {
  if (err && typeof err === 'object' && 'error' in err) {
    const inner = (err as { error: unknown }).error
    if (inner && typeof inner === 'object' && 'error' in inner) {
      return String((inner as { error: unknown }).error)
    }
  }
  return 'Unknown error'
}

function formatDate(iso?: string): string {
  if (!iso) return '—'
  const d = new Date(iso)
  return Number.isNaN(d.getTime()) ? iso : d.toLocaleString()
}

// A labelled read-only field used in the certificate detail grid.
function Field({ label, value }: { label: string; value: React.ReactNode }) {
  return (
    <div>
      <p className={labelCls}>{label}</p>
      <p className="text-sm break-all">{value}</p>
    </div>
  )
}

export default function ConnectionPage() {
  const { host, httpPort, wsPort, setHost, setHttpPort, setWsPort, api } = useConnection()
  const queryClient = useQueryClient()

  const certQuery = useQuery({
    queryKey: ['cert'],
    queryFn: () => api.getCert(),
  })
  const cert = certQuery.data?.data

  const refreshMutation = useMutation({
    mutationFn: () => api.refreshCert(),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['cert'] }),
  })

  // Expiry status drives the badge colour: expired (bad) → expiring soon (warn) → healthy (good).
  const status = cert
    ? cert.expired
      ? { label: `Expired ${Math.abs(cert.daysRemaining)} days ago`, cls: 'text-status-bad' }
      : cert.expiresSoon
        ? { label: `Expires in ${cert.daysRemaining} days`, cls: 'text-status-warn' }
        : { label: `Valid · ${cert.daysRemaining} days left`, cls: 'text-status-good' }
    : null

  return (
    <div>
      <PageHeader
        eyebrow="App"
        title="Connection"
        description="Configure the host and port used to reach the Motion Master backend."
      />
      <div className="p-4 sm:p-8 space-y-8">
        <section>
          <h2 className="eyebrow mb-3">Endpoint</h2>
          <div className="border border-grey-200 p-5">
            <div className="grid grid-cols-1 sm:grid-cols-2 gap-4">
              <div className="sm:col-span-2">
                <label className={labelCls}>Host</label>
                <input
                  type="text"
                  value={host}
                  onChange={e => setHost(e.target.value)}
                  placeholder="local.motion-master.synapticon.com"
                  className={inputCls}
                />
              </div>
              <div>
                <label className={labelCls}>HTTP Port</label>
                <input
                  type="text"
                  value={httpPort}
                  onChange={e => setHttpPort(e.target.value)}
                  placeholder="61447"
                  className={inputCls}
                />
              </div>
              <div>
                <label className={labelCls}>WebSocket Port</label>
                <input
                  type="text"
                  value={wsPort}
                  onChange={e => setWsPort(e.target.value)}
                  placeholder="62281"
                  className={inputCls}
                />
              </div>
            </div>
          </div>
        </section>

        <section>
          <div className="flex items-center justify-between mb-2">
            <h2 className="eyebrow">TLS Certificate</h2>
            <button
              onClick={() => refreshMutation.mutate()}
              disabled={refreshMutation.isPending}
              className={btnOutline}
            >
              {refreshMutation.isPending ? 'Refreshing…' : 'Refresh certificate'}
            </button>
          </div>
          <p className="text-xs text-grey-600 mb-4 max-w-prose">
            Downloads the latest certificate from Synapticon's rolling release, verifies it, and
            installs it next to the binary. It does not interrupt the running server — the new
            certificate takes effect the next time you restart Motion Master.
          </p>

          <div className="border border-grey-200 p-5 space-y-4">
            {certQuery.isLoading && <p className="text-sm text-grey-600">Loading…</p>}

            {certQuery.isError && (
              <p className="text-status-bad text-sm">
                Could not read certificate. Is the backend reachable at https://{host}:{httpPort}?
              </p>
            )}

            {cert && status && (
              <>
                <p className={`text-sm font-display font-medium ${status.cls}`}>{status.label}</p>
                <div className="grid grid-cols-1 sm:grid-cols-2 gap-4">
                  <Field label="Subject" value={cert.subject} />
                  <Field label="Issuer" value={cert.issuer} />
                  <Field label="Valid from" value={formatDate(cert.notBefore)} />
                  <Field label="Valid until" value={formatDate(cert.notAfter)} />
                  <Field label="File" value={cert.path} />
                </div>
              </>
            )}

            {refreshMutation.isSuccess && (
              <div className="border-l-2 border-status-info bg-status-info/10 px-3 py-2">
                <p className="text-xs font-display font-medium uppercase tracking-wide text-status-info">
                  Certificate updated
                </p>
                <p className="text-xs text-grey-700 mt-0.5">
                  A fresh certificate was installed. Restart Motion Master to start serving it.
                </p>
              </div>
            )}
            {refreshMutation.isError && (
              <p className="text-status-bad text-xs">{apiError(refreshMutation.error)}</p>
            )}
          </div>
        </section>
      </div>
    </div>
  )
}
