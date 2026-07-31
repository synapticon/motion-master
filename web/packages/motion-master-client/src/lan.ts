// Reaching a Motion Master that runs on another machine (an appliance on the workshop LAN, say)
// over TLS, from a page served at https://motion-master.synapticon.com.
//
// The obvious address — https://192.168.1.50:61447 — cannot work: no public CA will issue a
// certificate for a private IP, and a cross-origin fetch() offers no click-through for a
// self-signed one. The way out is a real, publicly-trusted name pointing at the private address.
// Names under `ip.motion-master.synapticon.com` write the address into their leftmost label, and
// the certificate every server ships carries the `*.ip.motion-master.synapticon.com` wildcard, so
// any such name validates with nothing to install.
//
// These names are resolved on the client machine by a hosts-file entry, not by public DNS — which
// is sufficient, because TLS checks a certificate against the name and never against how the name
// was resolved.
//
// The mapping is *offered* to the user, never applied behind their back: connecting straight to the
// IP address is a legitimate second path. It works once that origin has been opened in a tab and its
// certificate warning accepted, and it is the only path when the Console is being viewed from a
// phone or tablet — those cannot edit a hosts file, and it is the *browser's* machine that needs
// the entry, not the one running Motion Master. So these helpers convert on request; the caller
// decides.

// The subdomain whose leftmost label encodes an IPv4 address.
export const LAN_DOMAIN = 'ip.motion-master.synapticon.com';

// True when `value` is a dotted-quad IPv4 literal (each octet 0-255). Hostnames, IPv6 literals,
// and anything with a port or scheme attached are not.
export function isIpv4(value: string): boolean {
  const octets = value.trim().split('.');
  if (octets.length !== 4) {
    return false;
  }
  return octets.every((octet) => /^\d{1,3}$/.test(octet) && Number(octet) <= 255);
}

// Map a host to the name the bundled certificate covers: an IPv4 literal becomes its dashed form
// under LAN_DOMAIN (192.168.1.50 -> 192-168-1-50.ip.motion-master.synapticon.com); anything else
// is already a hostname and is returned trimmed but otherwise untouched — so a user who types
// `local.motion-master.synapticon.com`, or the dashed name itself, is left alone.
//
// Loopback is not special-cased: 127.0.0.1 maps to 127-0-0-1.ip.… which the same certificate
// covers. For a same-machine server the shorter `local.motion-master.synapticon.com` is still the
// better answer, since it is a public A record and so needs no hosts entry at all.
export function lanHostname(host: string): string {
  const trimmed = host.trim();
  if (!isIpv4(trimmed)) {
    return trimmed;
  }
  return `${trimmed.split('.').join('-')}.${LAN_DOMAIN}`;
}

// The inverse of lanHostname: recovers the address a LAN hostname encodes, or null if the host is
// not one of ours. Used to build the hosts-file line the user needs, since these names resolve
// nowhere until they add it.
export function lanAddress(host: string): string | null {
  const trimmed = host.trim().toLowerCase();
  const suffix = `.${LAN_DOMAIN}`;
  if (!trimmed.endsWith(suffix)) {
    return null;
  }
  const label = trimmed.slice(0, -suffix.length);
  const address = label.split('-').join('.');
  return isIpv4(address) ? address : null;
}
