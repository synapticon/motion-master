# Running Motion Master on another machine

Motion Master normally runs on the same machine as the browser. This document covers the other
arrangement: the server on a machine wired to the drives — a Raspberry Pi, an industrial PC — driven
from the Console on a laptop across the network.

Two things are needed: the server has to listen on the network, and the browser has to reach it over
HTTPS with a certificate it trusts. The first is one config key. The second is the interesting part.

**Running on a remote machine costs two manual steps**, deliberately, in exchange for needing no DNS
infrastructure and no certificate of your own:

1. **One line in the hosts file of each client machine** that will open the Console (Part 2).
2. **Keeping `cert.pem` / `key.pem` current on the server** — automatic wherever it has internet
   access, a periodic file copy where it does not (Certificates, below).

> **Motion Master has no authentication.** Anything that can reach these ports can enable drives and
> command motion. Only bind off loopback on a network you trust, and never expose these ports to the
> internet.

## Why the browser needs more than an IP address

The Console is served from `https://motion-master.synapticon.com`, a secure origin, so every request
it makes to the backend must pass TLS validation. `https://192.168.1.50:61447` cannot: no public CA
will issue a certificate for a private address, and a cross-origin `fetch()` gives the user no
chance to click through a self-signed warning — the request just fails.

The fix is a real, publicly-trusted *name* that points at the private address. Motion Master's
bundled certificate covers `*.ip.motion-master.synapticon.com`, and by convention the address is
written into the leftmost label with dashes:

```text
192.168.1.50   ->   192-168-1-50.ip.motion-master.synapticon.com
```

**That name is not resolved by public DNS — it is resolved on the client machine, by a hosts-file
entry.** This is the whole mechanism, and it is worth understanding why it is enough: TLS validates
a certificate against the *name* — chain of trust, SAN match, expiry — and never against how that
name was resolved. A locally-resolved name therefore yields exactly the same genuine,
publicly-trusted HTTPS connection as a globally-resolved one. No warning, nothing to click through,
nothing to install.

## Part 1 — the server machine

Install as usual (see [SETUP-linux.md](../SETUP-linux.md); on a Raspberry Pi use the `-arm64.deb`,
which needs Raspberry Pi OS **trixie** — the binaries require glibc 2.38). Then put a
`motion-master.jsonc` next to the binary — it is auto-discovered, no flag needed:

```jsonc
{
  "server": {
    // Serve the network, not just this machine.
    "bindAddress": "0.0.0.0"
  }
}
```

Restart. The log confirms the bind and what it means:

```text
[info] HTTP server listening on 0.0.0.0:61447
[info] WebSocket server listening on 0.0.0.0:62281
[warning] Bound off loopback — reachable from the network, and the API has NO authentication. Use only on a trusted network.
[info] Browsers reach this server as https://<dashed-ip>.ip.motion-master.synapticon.com:61447 (192-168-1-50.ip.… for 192.168.1.50) and need a matching hosts-file entry — see docs/LAN_DEPLOYMENT.md
```

Note the address itself is not printed. A machine usually has several (wired, wireless, a container
bridge) and only one of them is the one you want, so the log would be guessing; `hostname -I` or
`ip -4 addr` tells you, and the Console builds the name for you once you enter it.

Give the machine a **fixed address** — a DHCP reservation is enough — so the name stays valid.
Nothing else is needed on the device: the certificate it already ships with covers the name.

There is no discovery service, and none is planned: nothing advertises itself on the network. Read
the address off the machine as above, or from the router's lease list. A `.local` name would not
have helped you connect in any case — no CA issues for a reserved TLD, so it could only ever have
revealed the address you then have to type anyway.

## Part 2 — the client machine

Open the Console, go to **Connection**, and type the server's IP address in the **Host** field.
From there you have two ways to connect, and the page offers both rather than choosing for you.
Ports are unchanged (61447 / 62281), and the committed endpoint persists in that browser.

### Option A — use the hostname (no warnings)

The page shows the equivalent hostname under the entered address with a **Use hostname** button:

```text
192.168.1.50   →   192-168-1-50.ip.motion-master.synapticon.com
```

Press it, then **Apply**. For that name to resolve, add one line to the hosts file **of the computer
running the browser** — your laptop, not the server:

> Name resolution happens on the machine that makes the request. Your browser has to turn the
> hostname into an address before it can open a connection, and that lookup runs locally. The
> server never resolves its own name — it only listens on a socket and answers whatever arrives —
> so a hosts entry on the Raspberry Pi would achieve nothing. The practical consequence: three
> laptops connecting to one server need the line three times, once each, and it cannot be baked
> into a device image.

A script in this repository does it for you. It is not shipped with the release, because it belongs
on the client rather than the server — download it onto the computer you browse from:

**Linux, macOS** — in a terminal:

```bash
curl -fsSLO https://raw.githubusercontent.com/synapticon/motion-master/main/add-host.sh
sudo bash add-host.sh 192.168.1.50
```

**Windows** — in PowerShell started with *Run as administrator*:

```powershell
Invoke-WebRequest https://raw.githubusercontent.com/synapticon/motion-master/main/add-host.ps1 -OutFile add-host.ps1
.\add-host.ps1 192.168.1.50
```

Both take the server's IP address, write the matching line, back up the previous file, and print the
URL to open. Re-running is harmless, and `--remove` / `-Remove` takes the entry out again.

Doing it by hand is one line, if you prefer:

```text
192.168.1.50    192-168-1-50.ip.motion-master.synapticon.com
```

| OS | Path |
| --- | --- |
| Linux, macOS | `/etc/hosts` |
| Windows | `C:\Windows\System32\drivers\etc\hosts` |

Either way it needs administrator rights, and editing a hosts file sometimes trips
endpoint-protection software. A phone or tablet **viewing** the Console cannot do it at all — use
Option B there. In exchange the connection is ordinary trusted HTTPS, with nothing to accept and
nothing to re-do later.

If the endpoint is unreachable, the Connection page shows the exact line for that address with a
copy button.

### Chrome asks for local network access — allow it

On the first connection to a server on your network, Chrome shows a **local network access**
prompt. Allow it.

This is separate from everything above and cannot be avoided by getting the certificate or the
hostname right. Since Chrome 142 a page served from a public address — which the Console is — has
to ask permission before it may reach a private one, which is what prevents an arbitrary website
from probing the devices on your network. The check is on the **address the name resolves to**, not
the name, so a genuine certificate does not exempt it.

Granting it once covers the Console from then on. **Denying it makes requests fail silently**,
indistinguishable from a server that is not running — so if the connection fails with everything
else correct, check the permission via the icon at the left of Chrome's address bar. On managed
browsers an administrator can grant it fleet-wide with the `LocalNetworkAccessAllowedForUrls`
policy for `https://motion-master.synapticon.com`, and nobody is prompted.

### Option B — use the IP address directly (one browser warning)

Leave the address as you typed it and press **Apply**. Nothing needs to be installed or edited, but
the certificate cannot match a bare IP, so the browser rejects it — and because the Console requests
the API *cross-origin*, it cannot show you a warning to click through. It simply fails.

The way through is to grant the exception yourself, once per browser and device:

1. Open `https://192.168.1.50:61447` in a new tab.
2. Accept the certificate warning (**Advanced → Proceed**).
3. Return to the Console. Its requests to that origin now succeed.

Then repeat for `https://192.168.1.50:62281`. The WebSocket runs on its own port, and browsers key
these exceptions by host **and** port, so without it the Console loads and then sits with no live
data — the most confusing possible half-working state. That page will show an error or a blank
response once you accept, because the port speaks WebSocket rather than HTTP; that is expected, and
the exception is recorded regardless.

**Which to choose.** Option A on a computer you will use regularly: nothing to re-accept, no
warnings. Option B for a quick look, where you lack administrator rights on the client, or when the
Console is being viewed from a phone or tablet — there it is the only possibility, since those
cannot edit a hosts file, though how long a browser keeps an accepted exception varies, so expect
to grant it again.

## Certificates

Nothing per-device: **one** certificate covers both deployments, and every install serves the same
file.

| Name | Used when |
| --- | --- |
| `local.motion-master.synapticon.com` | server and browser on the same machine (a public A record for `127.0.0.1`) |
| `*.ip.motion-master.synapticon.com` | server on another machine, resolved by a hosts entry |

They are issued together as two SANs by `cert-renewal.yml` and published to the rolling `tls-cert`
release.

**Keeping them current is the second manual step for an offline machine.** Let's Encrypt
certificates last 90 days, and once one expires the browser refuses the connection outright — there
is no click-through for a cross-origin request.

| The server can reach the internet | It cannot |
| --- | --- |
| Nothing to do. The startup self-heal fetches a fresh certificate when the current one is missing, expired, or within 7 days of expiring, and **Refresh certificate** on the Connection page does it on demand. | Copy `cert.pem` and `key.pem` next to the binary before the current one lapses, from `https://github.com/synapticon/motion-master/releases/download/tls-cert/`, and restart. |

There is no way around the periodic copy for an air-gapped machine, and it is not worth trying:
**no certificate authority can issue a long-lived certificate.** The public maximum is 200 days, and
falls to 100 days in March 2027 and 47 in March 2029. Even a private CA or a self-signed
certificate is capped, because Apple rejects *any* TLS server certificate valid for more than 825
days regardless of who signed it. Two years is the ceiling anywhere, and only if every client
machine has your root installed.

### None of this applies if you only use the API

Everything above — the hostname, the hosts-file entry, the certificate and its expiry — exists for
one reason: **so that a browser connects without a warning.** A program does not care. `curl -k`,
Python's `requests(verify=False)` and Node's `rejectUnauthorized: false` all connect happily to any
certificate, whatever name it carries and whether or not it has expired:

```bash
curl -k https://192.168.1.50:61447/api/version
```

So a server driven only by scripts, a test rig, or CI needs **no** DNS name, no hosts entry, and no
valid certificate — just `bindAddress` set to `0.0.0.0` and the address. The certificate it happens
to be serving is irrelevant, and letting it expire breaks nothing.

The one thing still required is that *some* certificate file exists, because both listeners are
TLS-only and the server will not start without one. Our own certificate is the easiest way to
satisfy that — download it once and copy it next to the binary:

```bash
curl -LO https://github.com/synapticon/motion-master/releases/download/tls-cert/cert.pem
curl -LO https://github.com/synapticon/motion-master/releases/download/tls-cert/key.pem
```

Then **let it expire**. For an API-only server that is not a problem to manage: no client is
checking it, so nothing breaks when it lapses. Set `tls.autoUpdate` to `false` if the machine has no
internet, so startup does not spend time on a fetch that cannot succeed.

### The one-time DNS requirement

Issuing a wildcard requires proving control of `ip.motion-master.synapticon.com` over DNS-01, so one
static record is needed in the `synapticon.com` zone — **once, ever**, not per device:

| Name | RR Type | Value |
| --- | --- | --- |
| `_acme-challenge.ip.motion-master` | `CNAME` | `4723b93a-99f5-43d7-93f1-195dbb4168ea.auth.acme-dns.io.` |

That is the same acme-dns account the existing `local.…` name uses, so no new secret is involved.
acme-dns keeps two rolling TXT records per account and this certificate needs exactly two — which is
also the ceiling: a third name on the certificate would need a second acme-dns account.

Then run the **Renew TLS Certificate** workflow and confirm both names landed:

```bash
curl -sL https://github.com/synapticon/motion-master/releases/download/tls-cert/cert.pem \
  | openssl x509 -noout -ext subjectAltName
```

No `A` records, no delegation, and no nameserver to operate — the `ip.…` names deliberately do not
resolve in public DNS. A pleasant side effect: because they resolve nowhere, nobody can point one at
a host of their own and serve trusted HTTPS under a `synapticon.com` name, even though the
certificate's private key is published with every release.

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| Connection refused / no response | Server still bound to loopback — check the startup log for `0.0.0.0` — or a firewall on the device. |
| Everything looks correct but nothing connects | Chrome's **local network access** permission was denied for `https://motion-master.synapticon.com`; denied requests fail silently. Check the icon at the left of the address bar. |
| Name does not resolve | The hosts entry is missing, has a typo, or the address is not the one the server logged. Check with `getent hosts <name>` (Linux), `dscacheutil -q host -a name <name>` (macOS), or `ping <name>` (Windows). |
| Certificate error in the browser | The served certificate predates the wildcard SAN. Check **Connection → Valid for** in the Console (or `GET /api/cert`); press **Refresh certificate**, or restart so the startup self-heal fetches the current one. |
| Console reaches the API but shows no live data | The WebSocket port (62281) is blocked while the HTTP port is not — both must be reachable. |
| Worked, then stopped | The device's DHCP lease changed its address, so the name now points at the wrong host. Give it a reservation and update the hosts entry. |
