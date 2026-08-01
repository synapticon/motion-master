import Explainer from './Explainer';

// The teaching panel for the App → Connection page. It carries the full story behind every control
// on that page — why a hostname is needed at all, which machine has to resolve it, how the single
// bundled certificate covers both the local and the LAN case, and what the read-only panels show —
// so the controls themselves need only a line of context each.
export default function ConnectionExplainer() {
  return (
    <Explainer title="How does the Console reach Motion Master?">
      <p>
        This Console is a web app served from{' '}
        <code>https://motion-master.synapticon.com</code>. Motion Master itself runs on your own
        hardware and is a <em>different</em> origin, so every request the page makes to it is a{' '}
        <strong>cross-origin</strong> request over HTTPS — and the browser will refuse it outright
        unless the backend presents a certificate that is both publicly trusted and issued for the
        exact name you are connecting to. Crucially there is <strong>no click-through</strong>: the
        warning page you get when browsing to a site with a bad certificate does not exist for
        background requests. They simply fail.
      </p>

      <p>
        Everything below follows from that one constraint. Two ports are involved and both must be
        reachable: <code>61447</code> for the HTTP API and <code>62281</code> for the WebSocket that
        carries live monitoring data. They run on separate threads so a slow request can never stall
        the live stream.
      </p>

      <dl className="space-y-3">
        <div>
          <dt className="font-medium text-grey-900">
            Motion Master on this same machine — the default
          </dt>
          <dd>
            <code>local.motion-master.synapticon.com</code> is a real, publicly registered DNS
            record that resolves to <code>127.0.0.1</code> — your own machine. Traffic never leaves
            the loopback interface, but because the name is genuine, Synapticon can obtain a real
            CA-issued certificate for it, which Motion Master then serves locally. Nothing to set
            up: this is what a normal desktop install uses.
          </dd>
        </div>

        <div>
          <dt className="font-medium text-grey-900">
            Motion Master on another machine — a Raspberry Pi or industrial PC
          </dt>
          <dd>
            <p>
              First, that machine must be listening to the network: set{' '}
              <code>server.bindAddress</code> to <code>0.0.0.0</code> in its config file, since the
              default of <code>127.0.0.1</code> accepts connections only from itself. Note the API
              has <strong>no authentication</strong> — anything that can reach it can command
              motion — so do this only on a network you trust.
            </p>
            <p className="mt-2">
              Second, the browser needs a name. No certificate authority will issue one for a bare
              IP address, so each address has an equivalent hostname that the bundled certificate{' '}
              <em>does</em> cover, with the address written into the first label:
            </p>
            <p className="mt-2">
              <code>192.168.1.50</code> → <code>192-168-1-50.ip.motion-master.synapticon.com</code>
            </p>
          </dd>
        </div>

        <div>
          <dt className="font-medium text-grey-900">
            Making that hostname resolve — on <em>this</em> computer
          </dt>
          <dd>
            <p>
              Those <code>ip.…</code> names are deliberately absent from public DNS, so you map one
              to its address in the hosts file of the machine running the browser:
            </p>
            <p className="mt-2">
              <code>/etc/hosts</code> on Linux and macOS,{' '}
              <code>C:\Windows\System32\drivers\etc\hosts</code> on Windows. The{' '}
              <code>add-host.sh</code> and <code>add-host.ps1</code> scripts in the Motion Master
              repository write the line for you.
            </p>
            <p className="mt-2">
              It has to be <strong>this</strong> computer, not the server. Name resolution happens
              at the end that makes the request: your browser turns the hostname into an address
              before it can open a connection, and that lookup runs here. The server never looks up
              its own name, so an entry there would achieve nothing — which also means every
              computer that opens this Console needs its own line, and it cannot be baked into a
              device image.
            </p>
            <p className="mt-2">
              This costs nothing in security. TLS validates a certificate against the{' '}
              <strong>name</strong>, never against how that name was resolved, so a locally resolved
              name yields exactly the same genuine, publicly trusted connection.
            </p>
          </dd>
        </div>

        <div>
          <dt className="font-medium text-grey-900">
            Or skip the hostname and use the IP address
          </dt>
          <dd>
            Entering the plain address works too, and needs no administrator rights — useful on a
            locked-down machine, and the only option on a phone or tablet, which cannot edit a hosts
            file. The catch is that you must grant the certificate exception yourself, because this
            page cannot prompt for one: open <code>https://&lt;address&gt;:61447</code> in a tab,
            accept the warning, then repeat for <code>https://&lt;address&gt;:62281</code>. Browsers
            store these per host <em>and</em> port, so skipping the second leaves the Console
            loading with no live data — the most confusing possible half-working state. That second
            page will show an error after you accept, because the port speaks WebSocket rather than
            HTTP; the exception is still recorded.
          </dd>
        </div>

        <div>
          <dt className="font-medium text-grey-900">
            Chrome will ask permission to reach your local network
          </dt>
          <dd>
            <p>
              On the first connection to a server on your network, Chrome shows a{' '}
              <strong>local network access</strong> prompt. Allow it. This is unrelated to the
              certificate: since Chrome 142, a page served from a public address — which this
              Console is — must ask before it may reach a private one, which is what stops an
              arbitrary website from probing the devices on your network.
            </p>
            <p className="mt-2">
              The check is on the <em>address</em> the name resolves to, not the name itself, so a
              genuine certificate and a real hostname do not exempt it. Granting it once covers this
              Console from then on. <strong>Denying it makes requests fail silently</strong>, which
              looks exactly like a server that is not running — so if the connection fails and
              everything else checks out, look at the permission (the icon at the left of Chrome&apos;s
              address bar).
            </p>
            <p className="mt-2">
              On managed browsers an administrator can grant it centrally with the{' '}
              <code>LocalNetworkAccessAllowedForUrls</code> policy for{' '}
              <code>https://motion-master.synapticon.com</code>, so nobody is prompted at all.
            </p>
          </dd>
        </div>

        <div>
          <dt className="font-medium text-grey-900">The certificate</dt>
          <dd>
            <p>
              One certificate covers both cases — <code>local.motion-master.synapticon.com</code>{' '}
              and every <code>*.ip.motion-master.synapticon.com</code> name — so every install
              serves the same file and there is nothing per-machine to configure. The{' '}
              <strong>TLS Certificate</strong> section lists those names under{' '}
              <strong>Valid for</strong>: if the host you entered is not among them, the browser
              will reject the connection.
            </p>
            <p className="mt-2">
              Let&apos;s Encrypt certificates last 90 days. A server with internet access keeps
              itself current: it fetches a fresh one at startup when the current one is missing,
              expired, or within a week of expiring, and <strong>Refresh certificate</strong> does
              it on demand. Setting <code>tls.autoUpdate</code> to <code>false</code> in the config
              file turns that off for air-gapped installs, which instead need{' '}
              <code>cert.pem</code> and <code>key.pem</code> copied next to the binary periodically.
            </p>
            <p className="mt-2">
              Once a certificate has actually expired, the button can no longer help — this page
              cannot reach the backend over a trusted connection to ask. Restart the server and its
              startup fetch will heal it; with auto-update disabled, run{' '}
              <code>motion-master --update-cert</code> first.
            </p>
            <p className="mt-2">
              None of this matters if you only drive Motion Master over the API. Scripts skip
              certificate verification (<code>curl -k</code>, <code>verify=False</code>), so the
              name and the expiry are irrelevant there.
            </p>
          </dd>
        </div>

        <div>
          <dt className="font-medium text-grey-900">Started configuration and System</dt>
          <dd>
            <strong>Started configuration</strong> is the fully-resolved configuration the server
            booted with: a JSONC config file overlaid on the built-in defaults. That file is the
            only way to set ports, the fieldbus driver and adapter, log level, the recorder and TLS
            — there are no command-line flags for them. It is captured at startup and never re-read,
            so changing a value means editing the file and restarting. <strong>System</strong> is a
            live snapshot of the OS and hardware the backend runs on, which is mostly useful when
            that is a different machine; fields the platform cannot report show as <code>—</code>.
          </dd>
        </div>
      </dl>
    </Explainer>
  );
}
