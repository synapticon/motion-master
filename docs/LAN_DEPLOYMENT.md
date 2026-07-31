# Running Motion Master on another machine

Motion Master normally runs on the same machine as the browser. This document covers the other
arrangement: the server on a dedicated machine wired to the drives — a Raspberry Pi, an industrial
PC — driven from the Console on a laptop across the network.

## Why an IP address is not enough

The Console is served from `https://motion-master.synapticon.com`, a secure origin, so every request
it makes to the backend must pass TLS validation. `https://192.168.1.50:61447` cannot: no public CA
will issue a certificate for a private address, and a cross-origin `fetch()` offers no
click-through for a self-signed one — the request simply fails, with no way for the user to accept
a warning.

The way out is a real, publicly-trusted hostname that happens to resolve to the private address.
That is already how the localhost case works (`local.motion-master.synapticon.com` is a public `A`
record for `127.0.0.1`), and it generalises: names under `ip.motion-master.synapticon.com` encode
the address in their leftmost label.

```
192-168-1-50.ip.motion-master.synapticon.com   →   A   192.168.1.50
```

The certificate every server ships carries `*.ip.motion-master.synapticon.com`, so all such names
validate with nothing to install or register per device.

> **Motion Master has no authentication.** Anything that can reach these ports can enable drives and
> command motion. Only bind off loopback on a network you trust, and never expose these ports to the
> internet.

## Part 1 — DNS (one-time, for the whole fleet)

This is infrastructure work in the `synapticon.com` zone, not something the server does. It is
required once; every device afterwards needs nothing.

### 1.1 Make `ip.motion-master.synapticon.com` resolve

Two ways, in order of preference.

**Option A — a scripted zone in dnsBOX (preferred).** If the appliance runs PowerDNS with Lua
records (or the pipe backend), it can synthesize the answer itself from the queried label. Nothing
is delegated, no host has to be run, and `_acme-challenge.ip.…` below stays an ordinary record in
the same zone. **Check the appliance's backend before building anything else.**

**Option B — delegate to CoreDNS.** Add `NS` records for `ip.motion-master.synapticon.com` pointing
at a host you run (ideally two, for redundancy), serving:

```
ip.motion-master.synapticon.com {
    # Synthesize A answers from the dashed address in the leftmost label.
    template IN A {
        match "^(?P<a>[0-9]{1,3})-(?P<b>[0-9]{1,3})-(?P<c>[0-9]{1,3})-(?P<d>[0-9]{1,3})\.ip\.motion-master\.synapticon\.com\.$"
        answer "{{ .Name }} 60 IN A {{ .Group.a }}.{{ .Group.b }}.{{ .Group.c }}.{{ .Group.d }}"
        fallthrough
    }
    # The DNS-01 challenge for the wildcard. Once the zone is delegated, the parent can no longer
    # answer for anything below it, so this record has to live here.
    file /etc/coredns/ip.zone
}
```

Stock **sslip.io is not sufficient** for Option B: it answers only A/AAAA/NS/SOA/MX, so a zone
delegated to it has nowhere to serve the ACME challenge from.

### 1.2 Add the ACME challenge CNAME

The wildcard is issued over DNS-01, which needs a TXT record under the delegated name. Reuse the
existing acme-dns account:

```
_acme-challenge.ip.motion-master.synapticon.com.  CNAME  4723b93a-99f5-43d7-93f1-195dbb4168ea.auth.acme-dns.io.
```

With Option A this is a record in the parent zone; with Option B it belongs in the delegated zone
file. No new secret is needed — `cert-renewal.yml` already holds these credentials.

> acme-dns keeps **two** rolling TXT records per account. This certificate has two names and
> therefore needs exactly two — the ceiling. Adding a third SAN later requires a second acme-dns
> account and its own CNAME.

### 1.3 Issue the certificate

Run the **Renew TLS Certificate** workflow (`workflow_dispatch`). It issues both names as one
certificate, verifies the SANs, and publishes to the rolling `tls-cert` release. Confirm with:

```bash
curl -sL https://github.com/synapticon/motion-master/releases/download/tls-cert/cert.pem \
  | openssl x509 -noout -ext subjectAltName
```

Both `local.motion-master.synapticon.com` and `*.ip.motion-master.synapticon.com` must appear.

## Part 2 — the server machine

Install as usual (see `SETUP-linux.md`; on a Raspberry Pi use the `-arm64.deb`, which needs
Raspberry Pi OS **trixie** — the binaries require glibc 2.38). Then place a
`motion-master.jsonc` next to the binary — it is auto-discovered, no flag needed:

```jsonc
{
  "server": {
    // Serve the network, not just this machine.
    "bindAddress": "0.0.0.0"
  }
}
```

Restart and confirm the log shows the new address:

```
HTTP server listening on 0.0.0.0:61447
WebSocket server listening on 0.0.0.0:62281
```

Give the machine a **fixed address** — a DHCP reservation is enough — so the hostname a user
bookmarks keeps working.

Nothing else is needed on the device. The certificate it already has (bundled with the release, or
fetched by the startup self-heal) covers its LAN name.

### Making it findable

The user still has to know the address. Configure mDNS on the device (Raspberry Pi OS ships Avahi)
so it answers to something like `motion-master.local`; a `ping motion-master.local` then reveals the
address to type into the Console. Note that the `.local` name itself cannot be used to connect — no
CA will issue for a reserved TLD — it is only a way to discover the address.

## Part 3 — connecting

In the Console, open **Connection** and enter the server's IP address in the **Host** field. The
page converts it to the certificate-covered hostname and shows what it will connect to before you
apply:

```
192.168.1.50  →  192-168-1-50.ip.motion-master.synapticon.com
```

Click **Apply**. Ports are unchanged (61447 / 62281). The endpoint persists in that browser.

## Troubleshooting

| Symptom | Cause |
|---|---|
| Connection refused / no response | Server still bound to loopback — check the startup log for `0.0.0.0`, and check a firewall on the device. |
| DNS does not resolve the dashed name | Part 1 incomplete, or a resolver with **DNS rebinding protection** dropping the private address from a public answer (common on consumer routers, `dnsmasq --stop-dns-rebind`). Test with `dig` against `8.8.8.8` to tell the two apart. |
| Certificate error in the browser | The served certificate predates the wildcard. Check **Connection → Valid for** in the Console (or `GET /api/cert`); press **Refresh certificate**, or restart the server so its startup self-heal fetches the current one. |
| Console reaches the API but no live data | The WebSocket port (62281) is blocked while the HTTP port is not — both must be reachable. |
