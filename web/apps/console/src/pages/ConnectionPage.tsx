import { useEffect, useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { isIpv4, lanAddress, lanHostname } from '@synapticon/motion-master-client';
import { version as uiVersion } from 'virtual:swagger-spec'
import Callout from '../components/Callout'
import ConnectionExplainer from '../components/ConnectionExplainer';
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { formatBytes } from '../utils/format'
import { btnOutline, btnPrimary } from '../utils/styles'

const RELEASES_URL = 'https://github.com/synapticon/motion-master/releases'

const inputCls = 'border border-grey-300 px-3 py-2 text-sm w-full bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'

// A valid TCP port is an integer in 1..65535. Guards the endpoint form so a typo (e.g. an extra
// digit → 622811) can't be persisted and then crash the app on the next load when the monitoring
// WebSocket URL is constructed from it.
function isValidPort(s: string): boolean {
  const n = Number(s.trim())
  return Number.isInteger(n) && n >= 1 && n <= 65535
}

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

// System sizes render as "—" when absent or zero, because the backend reports 0 for a value it
// could not determine on the current platform — unlike a file size, where 0 is a real answer. The
// formatting itself is the shared one, so a size reads the same here as on the storage pages.
function formatSystemBytes(bytes?: number): string {
  return bytes ? formatBytes(bytes) : '—'
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

  // The context host/httpPort/wsPort are the *committed* endpoint: they drive the API client and
  // the (app-wide, url-keyed) WebSocket connection. Editing them live would rebuild that socket on
  // every keystroke — remounting the whole app (so this input loses focus after one letter) and
  // constructing a WebSocket from half-typed values (e.g. `wss://…:622811`). So the fields edit a
  // local draft and only commit on Apply; the drafts re-sync whenever the committed endpoint
  // changes elsewhere (Load defaults).
  const [draftHost, setDraftHost] = useState(host)
  const [draftHttpPort, setDraftHttpPort] = useState(httpPort)
  const [draftWsPort, setDraftWsPort] = useState(wsPort)
  useEffect(() => {
    setDraftHost(host)
    setDraftHttpPort(httpPort)
    setDraftWsPort(wsPort)
  }, [host, httpPort, wsPort])

  // No certificate can be issued for a bare IP address, so an address typed here has an equivalent
  // hostname the bundled certificate does cover. It is *offered*, never substituted: connecting
  // straight to the IP is a legitimate choice (it works once the user has visited that origin in a
  // tab and accepted the warning), and silently rewriting what someone typed would take that away
  // and hide where the odd-looking hostname came from.
  const lanSuggestion = isIpv4(draftHost) ? lanHostname(draftHost) : null;

  const endpointDirty =
    draftHost.trim() !== host || draftHttpPort !== httpPort || draftWsPort !== wsPort
  const httpPortValid = isValidPort(draftHttpPort)
  const wsPortValid = isValidPort(draftWsPort)
  const endpointValid = draftHost.trim() !== '' && httpPortValid && wsPortValid
  const applyEndpoint = () => {
    if (!endpointValid) return
    setHost(draftHost.trim())
    setHttpPort(draftHttpPort.trim())
    setWsPort(draftWsPort.trim())
  }

  // When a LAN endpoint is unreachable, the most common cause is name resolution rather than the
  // server being down — so offer the hosts-file entry that settles it. Derived from the committed
  // host, not the draft, since that is the endpoint actually failing.
  const unreachableLanAddress = !online ? lanAddress(host) : null;
  const [hostsLineCopied, setHostsLineCopied] = useState(false);
  const hostsLine = unreachableLanAddress ? `${unreachableLanAddress}\t${host}` : '';
  const copyHostsLine = async () => {
    await navigator.clipboard.writeText(hostsLine);
    setHostsLineCopied(true);
    setTimeout(() => setHostsLineCopied(false), 2000);
  };

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
      <div className="p-4 sm:px-8 sm:py-7 space-y-6">
        <ConnectionExplainer />

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
          <h2 className="eyebrow mb-3">Endpoint</h2>
          <form
            className="border border-grey-200 p-5"
            onSubmit={e => {
              e.preventDefault()
              applyEndpoint()
            }}
          >
            <div className="grid grid-cols-1 sm:grid-cols-2 gap-4">
              <div className="sm:col-span-2">
                <label className={labelCls}>Host</label>
                <input
                  type="text"
                  value={draftHost}
                  onChange={e => setDraftHost(e.target.value)}
                  placeholder="local.motion-master.synapticon.com or 192.168.1.50"
                  className={inputCls}
                />
                {lanSuggestion && (
                  <div className="mt-2 border border-grey-200 bg-grey-50 p-3">
                    <p className="text-xs text-grey-700">
                      <code className="break-all">{lanSuggestion}</code> — the same address written
                      as a hostname the certificate covers.
                    </p>
                    {/* Directly under the name it produces, so the button reads as acting on the
                        line above it. Smaller than a primary action: this is an optional
                        convenience, not the way to submit the form. */}
                    <button
                      type="button"
                      onClick={() => setDraftHost(lanSuggestion)}
                      className={`${btnOutline} mt-2 px-2 py-1 text-[11px]`}
                      title="Only fills the Host field — press Apply to connect."
                    >
                      Replace IP with hostname
                    </button>
                    <p className="text-xs text-grey-600 mt-2 max-w-prose">
                      The hostname connects without warnings but needs a hosts-file entry on{' '}
                      <em>this</em> computer. The plain address needs none, but you must accept its
                      certificate warning yourself first. See the explainer above.
                    </p>
                  </div>
                )}
                <p className="text-xs text-grey-600 mt-1 max-w-prose">
                  Motion Master on this machine: leave the default. On another machine: enter its IP
                  address — it must be running with <code>server.bindAddress</code> set to{' '}
                  <code>0.0.0.0</code>.
                </p>
              </div>
              <div>
                <label className={labelCls}>HTTP Port</label>
                <input
                  type="text"
                  value={draftHttpPort}
                  onChange={e => setDraftHttpPort(e.target.value)}
                  placeholder="61447"
                  className={inputCls}
                  aria-invalid={!httpPortValid}
                />
                {!httpPortValid && (
                  <p className="text-xs text-status-bad mt-1">Must be a port number in 1–65535.</p>
                )}
              </div>
              <div>
                <label className={labelCls}>WebSocket Port</label>
                <input
                  type="text"
                  value={draftWsPort}
                  onChange={e => setDraftWsPort(e.target.value)}
                  placeholder="62281"
                  className={inputCls}
                  aria-invalid={!wsPortValid}
                />
                {!wsPortValid && (
                  <p className="text-xs text-status-bad mt-1">Must be a port number in 1–65535.</p>
                )}
              </div>
            </div>
            <div className="flex items-center justify-end gap-2 mt-4">
              <button
                type="button"
                onClick={resetEndpoint}
                className={btnOutline}
                title="Reset the host and ports to their built-in defaults (local.motion-master.synapticon.com, 61447, 62281) and save them to this browser's local storage, so they persist across reloads."
              >
                Load defaults
              </button>
              <button
                type="submit"
                disabled={!endpointDirty || !endpointValid}
                className={btnPrimary}
                title="Apply the host and ports above. This rebuilds the API client and the monitoring WebSocket to point at the new endpoint, so it only takes effect once you click here — not while you type."
              >
                Apply
              </button>
            </div>
          </form>

          {unreachableLanAddress && (
            <Callout variant="warning" className="mt-4">
              <p>
                <span className="font-display font-medium">Cannot reach {host}.</span> This name is
                not published in DNS, so <em>this</em> computer — the one running the browser, not{' '}
                {unreachableLanAddress} — needs one hosts-file line pointing the name at that
                address. That is not a workaround: it is how this connection is meant to work, and it
                costs nothing in security, because the certificate is checked against the name and
                never against how the name was resolved. You still get a genuine, publicly-trusted
                HTTPS connection with no warning.
              </p>
              <p className="mt-2">
                Also confirm that Motion Master on {unreachableLanAddress} has{' '}
                <code>server.bindAddress</code> set to <code>0.0.0.0</code> in its config file. It
                defaults to <code>127.0.0.1</code>, which accepts connections only from the machine
                Motion Master itself runs on — a server left at that default is unreachable from
                here however correct everything else is.
              </p>
              <p className="mt-2">
                And in Chrome, check that this site is allowed to reach your local network — the
                icon at the left of the address bar. Chrome asks before a public site may contact a
                private address; if that was denied, requests fail silently and look exactly like a
                server that is not running.
              </p>
              <div className="mt-2 flex items-center gap-2">
                <code className="bg-white/60 px-2 py-1 text-xs break-all">{hostsLine}</code>
                <button
                  type="button"
                  onClick={copyHostsLine}
                  className={btnOutline}
                  title="Copy the hosts-file line to the clipboard."
                >
                  {hostsLineCopied ? 'Copied' : 'Copy'}
                </button>
              </div>
              <p className="mt-2 text-xs">
                Add that line to <code>/etc/hosts</code> (Linux, macOS) or{' '}
                <code>C:\Windows\System32\drivers\etc\hosts</code> (Windows) — the{' '}
                <code>add-host.sh</code> and <code>add-host.ps1</code> scripts in the Motion Master
                repository will do it for you. Both need administrator rights. A phone or tablet
                cannot edit a hosts file at all; there, connect to{' '}
                <code>{unreachableLanAddress}</code> directly instead and accept the certificate
                warning.
              </p>
            </Callout>
          )}
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
                        {/* What the browser actually checks the host against — the Subject is
                            legacy and ignored. One certificate lists both the loopback name and
                            the wildcard covering LAN addresses, so this answers "is the host I
                            typed covered?" directly. */}
                        {cert.dnsNames && cert.dnsNames.length > 0 && (
                          <Field
                            label="Valid for"
                            value={
                              <span className="space-y-0.5 block">
                                {cert.dnsNames.map((name) => (
                                  <span key={name} className="block">
                                    {name}
                                  </span>
                                ))}
                              </span>
                            }
                          />
                        )}
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
                  Downloads the latest certificate, verifies it, and installs it next to the binary.
                  The running server is not interrupted — the new certificate takes effect on the
                  next restart. A restart alone usually suffices anyway, since Motion Master
                  refreshes an expiring certificate at startup. See the explainer above.
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
                  The configuration Motion Master booted with — the JSONC config file overlaid on the
                  built-in defaults. Read-only: to change a value, edit the file and restart.{' '}
                  <a
                    href="https://github.com/synapticon/motion-master/blob/main/apps/motion_master/motion-master.example.jsonc"
                    target="_blank"
                    rel="noreferrer"
                    className="text-ocean hover:underline"
                  >
                    motion-master.example.jsonc
                  </a>{' '}
                  documents every property.
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
                  The OS and hardware the backend is running on, read live each time this page
                  loads. Fields the platform cannot report show as <code>—</code>.
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
                    <Field label="Total memory" value={formatSystemBytes(system.totalMemoryBytes)} />
                    <Field
                      label="Disk (free / total)"
                      value={`${formatSystemBytes(system.diskFreeBytes)} / ${formatSystemBytes(system.diskTotalBytes)}`}
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
