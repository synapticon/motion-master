import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { version as uiVersion } from 'virtual:swagger-spec'
import Callout from '../components/Callout'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline } from '../utils/styles'

const RELEASES_URL = 'https://github.com/synapticon/motion-master/releases'

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

// Formats a byte count as a binary-prefix size (KiB/MiB/GiB/TiB). 0 renders as "—" since the
// backend reports 0 for a value it could not determine on the current platform.
function formatBytes(bytes?: number): string {
  if (!bytes) return '—'
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB']
  let value = bytes
  let unit = 0
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024
    unit++
  }
  return `${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`
}

// Combines the two related deployment facts into one cell: the container runtime MM runs inside (if
// any) and the host's Docker version. Folds the version into the runtime when that runtime is
// Docker to avoid a redundant "Docker · Docker 29.5.3".
function containerSummary(container?: string, dockerVersion?: string): string {
  const runtime = container
    ? container.charAt(0).toUpperCase() + container.slice(1)
    : 'None (bare metal)'
  if (!dockerVersion) return runtime
  if (container === 'docker') return `Docker ${dockerVersion}`
  return `Docker ${dockerVersion} · ${runtime}`
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

type ChainLink = { subject: string; issuer: string; organization: string; issuerOrganization: string }

// Renders the served certificate chain leaf-first. The PEM stops at the intermediate (the root
// lives in the OS trust store and is not transmitted), so we append a synthetic root node named by
// the last link's issuer — unless that link is self-signed (issuer == subject, e.g. a dev cert),
// in which case there is no separate root to show. Each entry shows the common name with the
// organization (O) beside it, since the friendly CA name ("Let's Encrypt") lives in O, not the CN.
function CertChain({ chain }: { chain: ChainLink[] }) {
  const last = chain[chain.length - 1]
  const rootName = last && last.issuer !== last.subject ? last.issuer : null
  const selfSigned = chain.length === 1 && chain[0].issuer === chain[0].subject
  const role = (i: number) => (selfSigned ? 'Self-signed' : i === 0 ? 'Leaf' : 'Intermediate CA')
  return (
    <div>
      <p className={labelCls}>Certificate chain</p>
      <ol className="text-sm space-y-1.5">
        {chain.map((link, i) => (
          <li key={i} className="break-all" style={{ paddingLeft: `${i * 1.25}rem` }}>
            <span className="text-xs uppercase tracking-wide text-grey-400 mr-2">{role(i)}</span>
            {link.subject}
            {link.organization && <span className="text-grey-500"> · {link.organization}</span>}
          </li>
        ))}
        {rootName && (
          <li className="break-all" style={{ paddingLeft: `${chain.length * 1.25}rem` }}>
            <span className="text-xs uppercase tracking-wide text-grey-400 mr-2">Root CA</span>
            {rootName}
            {last.issuerOrganization && (
              <span className="text-grey-500"> · {last.issuerOrganization}</span>
            )}
            <span className="text-xs text-grey-500 ml-1">(in your OS trust store)</span>
          </li>
        )}
      </ol>
    </div>
  )
}

export default function ConnectionPage() {
  const { host, httpPort, wsPort, setHost, setHttpPort, setWsPort, resetEndpoint, api, online } = useConnection()
  const queryClient = useQueryClient()

  const certQuery = useQuery({
    queryKey: ['cert'],
    queryFn: () => api.getCert(),
  })
  const cert = certQuery.data?.data

  const configQuery = useQuery({
    queryKey: ['startedConfig'],
    queryFn: () => api.getStartedConfig(),
  })
  const startedConfig = configQuery.data?.data

  const systemQuery = useQuery({
    queryKey: ['systemInfo'],
    queryFn: () => api.getSystemInfo(),
  })
  const system = systemQuery.data?.data

  const versionQuery = useQuery({
    queryKey: ['version'],
    queryFn: () => api.getVersion(),
  })
  const serverVersion = versionQuery.data?.data.version
  // Both this web app's build version (the swagger spec's info.version, bumped in lock-step with the
  // app by tools/bump-version.sh) and the server's reported version come from the same VERSION file,
  // so an exact string mismatch means the locally installed server is out of step with this PWA —
  // which Synapticon always serves at its latest release.
  const versionMismatch = !!serverVersion && serverVersion !== uiVersion

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
        description="Configure the host and ports used to reach the Motion Master backend, check the TLS certificate status and refresh it, review the configuration the backend started with, and inspect the OS and hardware it is running on."
      />
      <div className="p-4 sm:p-8 space-y-6">
        <div className="space-y-4">
          <Callout variant={versionMismatch ? 'warning' : 'info'}>
            <p>
              {versionMismatch ? (
                <>
                  <span className="font-display font-medium">Version mismatch.</span> This web app
                  is <code>v{uiVersion}</code>, but the Motion Master server it is connected to
                  reports <code>v{serverVersion}</code>. Mismatched versions can cause features here
                  to behave unexpectedly or fail — install the matching server release to bring them
                  back in sync.
                </>
              ) : serverVersion ? (
                <>
                  This web app and the connected Motion Master server are both{' '}
                  <code>v{uiVersion}</code>.
                </>
              ) : (
                <>
                  This web app is <code>v{uiVersion}</code>. Install the matching Motion Master
                  server release, or browse all releases below.
                </>
              )}
            </p>
            <p className="mt-0.5">
              <a
                href={`${RELEASES_URL}/tag/v${uiVersion}`}
                target="_blank"
                rel="noreferrer"
                className="text-ocean hover:underline"
              >
                Download v{uiVersion}
              </a>
              <span className="text-grey-400"> · </span>
              <a
                href={RELEASES_URL}
                target="_blank"
                rel="noreferrer"
                className="text-ocean hover:underline"
              >
                All releases
              </a>
            </p>
          </Callout>

          {!online && (
            <Callout variant="warning">
              <p>
                Not connected. No response from{' '}
                <code>
                  https://{host}:{httpPort}
                </code>
                {host === 'local.motion-master.synapticon.com' && (
                  <>
                    {' '}
                    (which resolves to <code>127.0.0.1</code>, your own computer)
                  </>
                )}
                .
              </p>
              <p className="mt-0.5">
                Make sure Motion Master is installed and running there. Or fix the host and ports
                below if the backend runs elsewhere.
              </p>
            </Callout>
          )}
        </div>

        <section>
          <div className="flex items-center justify-between mb-3">
            <h2 className="eyebrow">Endpoint</h2>
            <button
              onClick={resetEndpoint}
              className={btnOutline}
              title="Reset the host and ports to their built-in defaults (local.motion-master.synapticon.com, 61447, 62281) and save them to this browser's local storage, so they persist across reloads."
            >
              Load defaults
            </button>
          </div>
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
                <p className="text-xs text-grey-600 mt-1 max-w-prose">
                  <code>local.motion-master.synapticon.com</code> is a public DNS A record that
                  resolves to <code>127.0.0.1</code> — your own machine, where Motion Master runs.
                  Traffic never leaves the loopback interface, yet the address is a real, globally
                  registered hostname, so Synapticon can obtain a genuine CA-issued TLS certificate
                  for it that Motion Master serves locally.
                </p>
                <p className="text-xs text-grey-600 mt-1 max-w-prose">
                  This is what makes the connection work. This PWA is served from{' '}
                  <code>https://motion-master.synapticon.com</code>, a secure origin, so the browser
                  reaches the backend only over HTTPS and WSS (mixed-content blocking) and every
                  cross-origin request must pass certificate validation. A self-signed or{' '}
                  <code>localhost</code> certificate would be rejected outright, with no click-through
                  — only a publicly trusted certificate for a real hostname is accepted, which is
                  exactly what this DNS trick provides.
                </p>
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

        {online && (
          <>
            <section>
              <h2 className="eyebrow mb-3">TLS Certificate</h2>

              <div className="border border-grey-200 p-5 space-y-4">
                {certQuery.isLoading && <p className="text-sm text-grey-600">Loading…</p>}

                {certQuery.isError && (
                  <p className="text-status-bad text-sm">
                    Could not read certificate. Is the backend reachable at https://{host}:
                    {httpPort}?
                  </p>
                )}

                {cert && status && (
                  <>
                    <p className={`text-sm font-display font-medium ${status.cls}`}>
                      {status.label}
                    </p>
                    <div className="grid grid-cols-1 2xl:grid-cols-3 gap-4">
                      <div className="space-y-4 self-start">
                        <Field label="Subject" value={cert.subject} />
                        <Field label="Issuer" value={cert.issuer} />
                        <Field label="File" value={cert.path} />
                      </div>
                      <div className="space-y-4 self-start">
                        <Field label="Valid from" value={formatDate(cert.notBefore)} />
                        <Field label="Valid until" value={formatDate(cert.notAfter)} />
                      </div>
                      {cert.chain && cert.chain.length > 0 && <CertChain chain={cert.chain} />}
                    </div>
                  </>
                )}
              </div>

              <div className="mt-4">
                <button
                  onClick={() => refreshMutation.mutate()}
                  disabled={refreshMutation.isPending}
                  className={btnOutline}
                >
                  {refreshMutation.isPending ? 'Refreshing…' : 'Refresh certificate'}
                </button>
                <p className="text-xs text-grey-600 mt-2 max-w-prose">
                  Downloads the latest certificate from Synapticon's rolling release, verifies it,
                  and installs it next to the binary. It does not interrupt the running server — the
                  new certificate takes effect the next time you restart Motion Master.
                </p>
                <p className="text-xs text-grey-600 mt-2 max-w-prose">
                  Motion Master also self-heals on startup: if the certificate is missing or expired
                  it fetches a fresh one automatically (unless started with{' '}
                  <code>--no-cert-update</code>), so a restart normally suffices.
                </p>
                <p className="text-xs text-grey-600 mt-2 max-w-prose">
                  Once the certificate has already expired this button can no longer help — the PWA
                  itself can no longer reach the backend over a trusted connection. A plain restart
                  heals it via the startup fetch above; if Motion Master runs with{' '}
                  <code>--no-cert-update</code>, refresh explicitly first with{' '}
                  <code>motion-master --update-cert</code>, then restart.
                </p>

                {refreshMutation.isSuccess && (
                  <div className="border-l-2 border-status-info bg-status-info/10 px-3 py-2 mt-3">
                    <p className="text-xs font-display font-medium uppercase tracking-wide text-status-info">
                      Certificate updated
                    </p>
                    <p className="text-xs text-grey-700 mt-0.5">
                      A fresh certificate was installed. Restart Motion Master to start serving it.
                    </p>
                  </div>
                )}
                {refreshMutation.isError && (
                  <p className="text-status-bad text-xs mt-2">{apiError(refreshMutation.error)}</p>
                )}
              </div>
            </section>

            <section>
              <h2 className="eyebrow mb-3">Started Configuration</h2>
              <div className="border border-grey-200 p-5 space-y-3">
                <p className="text-xs text-grey-600 max-w-prose">
                  The fully-resolved configuration Motion Master booted with. A JSONC config file
                  (passed with <code>-c</code>/<code>--config</code>) is the single source for ports,
                  the fieldbus driver and adapter, log level, the recorder, and TLS paths — there are
                  no command-line flags for these. Any key the file omits keeps its built-in default,
                  so what you see here is the file overlaid on those defaults; with no config file at
                  all it is the pure defaults.
                </p>
                <p className="text-xs text-grey-600 max-w-prose">
                  Read-only and captured at startup: it is not re-read while running. To change a
                  value, edit the JSONC file and restart Motion Master. The annotated{' '}
                  <a
                    href="https://github.com/synapticon/motion-master/blob/main/apps/motion_master/motion-master.example.jsonc"
                    target="_blank"
                    rel="noreferrer"
                    className="text-ocean hover:underline"
                  >
                    motion-master.example.jsonc
                  </a>{' '}
                  documents every property with inline comments — unlike the resolved values shown
                  below, which carry no descriptions.
                </p>

                {configQuery.isLoading && <p className="text-sm text-grey-600">Loading…</p>}

                {configQuery.isError && (
                  <p className="text-status-bad text-sm">
                    Could not read configuration. Is the backend reachable at https://{host}:
                    {httpPort}?
                  </p>
                )}

                {startedConfig && (
                  <pre className="text-xs font-mono bg-grey-50 border border-grey-200 p-3 overflow-x-auto whitespace-pre">
                    {JSON.stringify(startedConfig, null, 2)}
                  </pre>
                )}
              </div>
            </section>

            <section>
              <h2 className="eyebrow mb-3">System</h2>
              <div className="border border-grey-200 p-5 space-y-3">
                <p className="text-xs text-grey-600 max-w-prose">
                  The operating system and hardware the Motion Master backend is running on — useful
                  when the server is on a separate machine (e.g. a Raspberry Pi appliance). A
                  snapshot read live from the host each time this page loads; fields that cannot be
                  determined on the host's platform are shown as <code>—</code>.
                </p>

                {systemQuery.isLoading && <p className="text-sm text-grey-600">Loading…</p>}

                {systemQuery.isError && (
                  <p className="text-status-bad text-sm">
                    Could not read system information. Is the backend reachable at https://{host}:
                    {httpPort}?
                  </p>
                )}

                {system && (
                  <div className="grid grid-cols-1 sm:grid-cols-2 2xl:grid-cols-3 gap-4">
                    <Field label="Operating system" value={system.osName || '—'} />
                    <Field label="Kernel / build" value={system.kernel || '—'} />
                    <Field label="Architecture" value={system.architecture || '—'} />
                    <Field label="Hostname" value={system.hostname || '—'} />
                    <Field label="CPU" value={system.cpuModel || '—'} />
                    <Field label="Logical cores" value={system.cpuCores || '—'} />
                    <Field label="Total memory" value={formatBytes(system.totalMemoryBytes)} />
                    <Field
                      label="Disk (free / total)"
                      value={`${formatBytes(system.diskFreeBytes)} / ${formatBytes(system.diskTotalBytes)}`}
                    />
                    <Field
                      label="Docker / Container"
                      value={containerSummary(system.container, system.dockerVersion)}
                    />
                  </div>
                )}
              </div>
            </section>
          </>
        )}
      </div>
    </div>
  )
}
