# Running Motion Master on another machine

Motion Master normally runs on the same machine as the browser. This document covers the other
arrangement. The server runs on a machine wired to the drives, such as a Raspberry Pi or an
industrial PC. You drive it from the Console on a laptop, across the network.

Two things must be true. The server has to listen on the network. The browser has to reach it over
HTTPS with a certificate that it trusts. The first is one configuration key. The second is the
interesting part.

**A remote machine costs two manual steps.** That is deliberate. In exchange you need no DNS
infrastructure and no certificate of your own.

1. **Add one line to the hosts file of each client machine** that opens the Console. See Part 2.
2. **Keep `cert.pem` and `key.pem` current on the server.** This is automatic wherever the server
   reaches the internet. Where it does not, it is a periodic file copy. See
   [Certificates](#certificates).

> **Motion Master has no authentication.** Anything that reaches these ports can enable a drive and
> command motion. Bind off loopback only on a network you trust. Never expose these ports to the
> internet.

## Why the browser needs more than an IP address

The Console is served from `https://motion-master.synapticon.com`, which is a secure origin. So
every request it makes to the backend must pass TLS validation. `https://192.168.1.50:61447` cannot
pass it, for two reasons. No public CA (certificate authority, the body that signs a certificate
browsers trust) issues a certificate for a private address. And a cross-origin `fetch()` gives the
user no chance to click through a warning about a self-signed certificate. The request simply
fails.

The fix is a real, publicly trusted *name* that points at the private address. The bundled
certificate covers `*.ip.motion-master.synapticon.com`. By convention you write the address into
the leftmost label, with dashes in place of the dots:

```text
192.168.1.50   ->   192-168-1-50.ip.motion-master.synapticon.com
```

**Public DNS does not resolve that name. The client machine resolves it, from a hosts-file entry.**
This is the whole mechanism. It is worth knowing why it is enough. TLS validates a certificate
against the *name*: the chain of trust, the SAN match and the expiry date. A SAN (Subject
Alternative Name) is the field that lists the names a certificate is valid for. TLS never validates
how the name was resolved. So a locally resolved name gives exactly the same genuine, publicly
trusted HTTPS connection as a globally resolved one. There is no warning, nothing to click through
and nothing to install.

## Part 1 — the server machine

Install as usual. See [SETUP-linux.md](../SETUP-linux.md). On a Raspberry Pi use the `-arm64.deb`,
which needs Raspberry Pi OS **trixie**, because the binaries require glibc 2.38.

**On a Raspberry Pi 5 there is a second route**, and it skips this whole part. `rt/image/build-rpi-image.sh`
builds a flashable card that arrives with a real-time kernel, isolated cores, Motion Master already
installed as a service, and the bind below already set. Its Wi-Fi is configured by editing one file
on the card before first boot, which matters because the wired port is the EtherCAT segment.
[RASPBERRY_PI.md](RASPBERRY_PI.md) is the guide for using a published image;
[`rt/README.md`](../rt/README.md) is how one is built.

Then put a `motion-master.jsonc` next to the binary. The server discovers it, so no flag is needed:

```jsonc
{
  "server": {
    // Serve the network, not just this machine.
    "bindAddress": "0.0.0.0"
  }
}
```

Restart the server. The log confirms the bind, and says what it means:

```text
[info] HTTP server listening on 0.0.0.0:61447
[info] WebSocket server listening on 0.0.0.0:62281
[warning] Bound off loopback — reachable from the network, and the API has NO authentication. Use only on a trusted network.
[info] Browsers reach this server as https://<dashed-ip>.ip.motion-master.synapticon.com:61447 (192-168-1-50.ip.… for 192.168.1.50) and need a matching hosts-file entry — see docs/LAN_DEPLOYMENT.md
```

The log does not print the address itself. A machine usually has several: a wired interface, a
wireless one, a container bridge. Only one of them is the one you want, so the log would guess.
Read the address with `hostname -I` or `ip -4 addr`. The Console builds the name for you once you
enter it.

Give the machine a **fixed address**, for which a DHCP reservation is enough, so that the name
stays valid. The machine needs nothing else. The certificate it already ships with covers the name.

There is no discovery service, and none is planned. Nothing advertises itself on the network. Read
the address off the machine as above, or from the lease list of the router. A `.local` name does
not help you connect either. No CA issues a certificate for a reserved top-level domain, so such a
name only reveals the address that you then have to type.

**An offline server also misses the auto-tuning executable.** The install scripts download that
file rather than a release shipping it. A machine with no internet access therefore installs
without it, which is not an error: the server runs, and only the auto-tuning endpoints fail. There
are two ways to add the file later. Run `./install-auto-tuning.sh` on the server, once it reaches
the internet. Or download the asset for its platform on another machine, rename it to
`auto-tuning`, and copy it next to the server binary. `SETUP-linux.md` covers both.

## Part 2 — the client machine

Open the Console and go to **Connection**. Type the server's IP address in the **Host** field. From
there you have two ways to connect. The page offers both rather than choosing for you. The ports do
not change, and stay 61447 and 62281. The committed endpoint persists in that browser.

### Option A — use the hostname, and get no warning

The page shows the equivalent hostname under the address you entered, with a **Use hostname**
button:

```text
192.168.1.50   →   192-168-1-50.ip.motion-master.synapticon.com
```

Press it, then press **Apply**. That name must resolve, so add one line to the hosts file **of the
computer that runs the browser**. That is your laptop, not the server.

> Name resolution happens on the machine that makes the request. Your browser must turn the
> hostname into an address before it opens a connection, and that lookup runs locally. The server
> never resolves its own name. It only listens on a socket and answers whatever arrives. So a hosts
> entry on the Raspberry Pi achieves nothing. There is a practical consequence. Three laptops that
> connect to one server need the line three times, once on each laptop. You cannot bake it into a
> device image.

A script in this repository writes the line for you. The release does not ship it, because it
belongs on the client rather than on the server. Download it onto the computer you browse from.

**Linux and macOS**, in a terminal:

```bash
curl -fsSLO https://raw.githubusercontent.com/synapticon/motion-master/main/add-host.sh
sudo bash add-host.sh 192.168.1.50
```

**Windows**, in a PowerShell started with *Run as administrator*:

```powershell
Invoke-WebRequest https://raw.githubusercontent.com/synapticon/motion-master/main/add-host.ps1 -OutFile add-host.ps1
.\add-host.ps1 192.168.1.50
```

Both scripts take the server's IP address. Each one writes the matching line, backs up the previous
file, and prints the URL to open. A second run does no harm. `--remove` on Linux and macOS, or
`-Remove` on Windows, takes the entry out again.

You can also write the line by hand:

```text
192.168.1.50    192-168-1-50.ip.motion-master.synapticon.com
```

| OS | Path |
| --- | --- |
| Linux, macOS | `/etc/hosts` |
| Windows | `C:\Windows\System32\drivers\etc\hosts` |

Either way, the edit needs administrator rights. An edit to a hosts file also trips some
endpoint-protection software. A phone or a tablet that **views** the Console cannot make the edit
at all, so use Option B there. In exchange, the connection is ordinary trusted HTTPS. There is
nothing to accept, and nothing to redo later.

If the endpoint is unreachable, the Connection page shows the exact line for that address, with a
copy button.

### Chrome asks for local network access — allow it

Chrome shows a **local network access** prompt on the first connection to a server on your network.
Allow it.

This prompt is separate from everything above. A correct certificate or a correct hostname does not
avoid it. Since Chrome 142, a page served from a public address must ask permission before it
reaches a private one. The Console is served from a public address. This is the check that stops an
arbitrary website from probing the devices on your network. The check tests **the address that the
name resolves to**, not the name, so a genuine certificate does not exempt it.

Grant it once and it covers the Console from then on. **A denial makes requests fail silently.**
The failure looks exactly like a server that is not running. So if the connection fails and
everything else is correct, check the permission. The icon at the left of Chrome's address bar
holds it. On a managed browser an administrator grants it fleet-wide with the
`LocalNetworkAccessAllowedForUrls` policy for `https://motion-master.synapticon.com`, and then
nobody is prompted.

### Option B — use the IP address directly, and accept one warning

Leave the address as you typed it and press **Apply**. You install nothing and edit nothing. But
the certificate cannot match a bare IP address, so the browser rejects it. The Console requests the
API *cross-origin*, so it cannot show you a warning to click through. It simply fails.

The way through is to grant the exception yourself, once for each browser and device:

1. Open `https://192.168.1.50:61447` in a new tab.
2. Accept the certificate warning: **Advanced**, then **Proceed**.
3. Return to the Console. Its requests to that origin now succeed.

Then repeat all three steps for `https://192.168.1.50:62281`. The WebSocket runs on its own port,
and a browser keys these exceptions by host **and** port. Without the second exception the Console
loads and then sits with no live data, which is the most confusing half-working state there is.
That second page shows an error or a blank response once you accept. That is expected, because the
port speaks WebSocket rather than HTTP. The browser records the exception either way.

**Which one to choose.** Use Option A on a computer you work on regularly. Nothing needs to be
re-accepted, and there are no warnings. Use Option B for a quick look, or where you have no
administrator rights on the client, or where the Console is viewed from a phone or a tablet. On a
phone or a tablet it is the only option, because those cannot edit a hosts file. Note that browsers
differ in how long they keep an accepted exception, so expect to grant it again.

## Certificates

Nothing is per-device. **One** certificate covers both deployments, and every install serves the
same file.

| Name | Used when |
| --- | --- |
| `local.motion-master.synapticon.com` | server and browser on the same machine (a public A record for `127.0.0.1`) |
| `*.ip.motion-master.synapticon.com` | server on another machine, resolved by a hosts entry |

`cert-renewal.yml` issues the two names together, as two SANs on one certificate. It publishes them
to the rolling `tls-cert` release.

**The second manual step for an offline machine is to keep them current.** A Let's Encrypt
certificate lasts 90 days. Once one expires, the browser refuses the connection outright. A
cross-origin request has no click-through.

| The server reaches the internet | It does not |
| --- | --- |
| Nothing to do. The startup self-heal fetches a fresh certificate when the current one is missing, expired, or within 7 days of expiry. **Refresh certificate** on the Connection page does the same on demand. | Copy `cert.pem` and `key.pem` next to the binary before the current one lapses, from `https://github.com/synapticon/motion-master/releases/download/tls-cert/`. Then restart. |

An air-gapped machine has no way around that periodic copy, and it is not worth an attempt.
**No certificate authority can issue a long-lived certificate.** The public maximum is 200 days. It
falls to 100 days in March 2027, and to 47 days in March 2029. A private CA and a self-signed
certificate are capped too, because Apple rejects *any* TLS server certificate valid for more than
825 days, whoever signed it. Two years is the ceiling anywhere, and only if every client machine
holds your root certificate.

### None of this applies if you only use the API

Everything above exists for one reason: **so that a browser connects without a warning.** That
covers the hostname, the hosts-file entry, the certificate and its expiry. A program does not care.
`curl -k`, Python's `requests(verify=False)` and Node's `rejectUnauthorized: false` connect to any
certificate. The name it carries does not matter, and neither does an expiry in the past:

```bash
curl -k https://192.168.1.50:61447/api/version
```

So a server driven only by scripts, a test rig or CI needs **no** DNS name, no hosts entry and no
valid certificate. It needs `bindAddress` set to `0.0.0.0`, and the address. Which certificate it
serves is irrelevant, and an expiry breaks nothing.

One thing is still required. *Some* certificate file must exist, because both listeners are TLS
only and the server does not start without one. Our own certificate is the easiest way to satisfy
that. Download it once and copy it next to the binary:

```bash
curl -LO https://github.com/synapticon/motion-master/releases/download/tls-cert/cert.pem
curl -LO https://github.com/synapticon/motion-master/releases/download/tls-cert/key.pem
```

Then **let it expire**. On an API-only server that needs no management, because no client checks
it, so nothing breaks when it lapses. Set `tls.autoUpdate` to `false` if the machine has no
internet access, so that startup does not spend time on a fetch that cannot succeed.

### The one-time DNS requirement

A wildcard certificate requires proof of control over `ip.motion-master.synapticon.com`. The proof
runs over DNS-01, an ACME challenge that is answered by a DNS record rather than by a web request.
So the `synapticon.com` zone needs one static record — **once, ever**, not once per device:

| Name | RR Type | Value |
| --- | --- | --- |
| `_acme-challenge.ip.motion-master` | `CNAME` | `4723b93a-99f5-43d7-93f1-195dbb4168ea.auth.acme-dns.io.` |

That is the same acme-dns account the existing `local.…` name uses, so it involves no new secret.
acme-dns is a small DNS server that answers only the challenge record. It keeps two rolling TXT
records for each account, and this certificate needs exactly two. That is also the ceiling: a third
name on the certificate would need a second acme-dns account.

Then run the **Renew TLS Certificate** workflow, and confirm that both names landed:

```bash
curl -sL https://github.com/synapticon/motion-master/releases/download/tls-cert/cert.pem \
  | openssl x509 -noout -ext subjectAltName
```

There are no `A` records, no delegation and no nameserver to operate. The `ip.…` names deliberately
do not resolve in public DNS. That has a pleasant side effect. Because they resolve nowhere, nobody
can point one at a host of their own and serve trusted HTTPS under a `synapticon.com` name. This
holds even though every release publishes the certificate's private key.

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| Connection refused, or no response | The server is still bound to loopback. Check the startup log for `0.0.0.0`. Otherwise a firewall on the device blocks the port. |
| Everything looks correct but nothing connects | Chrome's **local network access** permission was denied for `https://motion-master.synapticon.com`. A denied request fails silently. Check the icon at the left of the address bar. |
| The name does not resolve | The hosts entry is missing, or it has a typo, or the address is not the one the server logged. Check it with `getent hosts <name>` on Linux, `dscacheutil -q host -a name <name>` on macOS, or `ping <name>` on Windows. |
| A certificate error in the browser | The certificate being served is older than the wildcard SAN. Check **Connection → Valid for** in the Console, or `GET /api/cert`. Press **Refresh certificate**, or restart so that the startup self-heal fetches the current one. |
| The Console reaches the API but shows no live data | The WebSocket port 62281 is blocked and the HTTP port is not. Both must be reachable. |
| It worked, then stopped | The DHCP lease of the device changed its address, so the name now points at the wrong host. Give it a reservation, and update the hosts entry. |
