// Reaching a Motion Master that runs on another machine (an appliance on the workshop LAN, say)
// over TLS, from a page served at https://motion-master.synapticon.com.
//
// The obvious address — https://192.168.1.50:61447 — cannot work: no public CA will issue a
// certificate for a private IP, and a cross-origin fetch() offers no click-through for a
// self-signed one. The way out is a real, publicly-trusted name that happens to resolve to the
// private address. Names under `ip.motion-master.synapticon.com` encode the address in their
// leftmost label (192-168-1-50.ip.… resolves to 192.168.1.50), and the certificate every server
// ships carries the `*.ip.motion-master.synapticon.com` wildcard, so the name validates without
// anything to pre-register per device.
//
// So the user types the address they already know (from mDNS, the router, or the server's own
// startup log) and the client turns it into the name the certificate covers.

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
// Loopback is not a special case: 127.0.0.1 maps to 127-0-0-1.ip.… which resolves right back to
// 127.0.0.1 and is equally covered. Pointing at the shorter `local.motion-master.synapticon.com`
// is still preferable there — it is a static record, so it keeps working if the responder serving
// the ip.… subdomain is ever unreachable.
export function lanHostname(host: string): string {
  const trimmed = host.trim();
  if (!isIpv4(trimmed)) {
    return trimmed;
  }
  return `${trimmed.split('.').join('-')}.${LAN_DOMAIN}`;
}
