# EtherCAT Stack Licensing

Motion Master is licensed under **GPL v3**, so that it can use SOEM v2 under SOEM's own GPL v3 license.

## SOEM

### v1 (up to v1.4.0) — GPL v2 only

- **GPL v2 only** (not "or later")
- Has a linking exception. Compiling SOEM and linking it with other code does not by itself put the combined program under the GPL. This holds for static linking too. The exception covers only SOEM's own files. The SOEM source, including any changes to it, must still be made available under section 3 of GPL v2.
- **Apache 2.0 conflicts with GPL v2 only for code combined into SOEM itself.** The FSF states that Apache 2.0 and GPL v2 cannot be combined, because of the Apache 2.0 patent termination and indemnification terms. That applies to a work that must be distributed under GPL v2. The linking exception means that code which only links to SOEM does not have to be. The exception is the eCos exception 2.0 word for word, which the FSF describes as "an exception allowing linking to software not under the GPL". The exception does not cover code copied from SOEM into another file.
- Requires an EtherCAT Master License from Beckhoff, as every EtherCAT master does (see below)

### v2 (v2.0.0+) — GPL v3 or commercial

- **Dual-licensed: GPL v3 OR commercial** (rt-labs AB, contact: <sales@rt-labs.com>)
- GPL v3 + Apache 2.0: compatible, but the combined work must be distributed as GPL v3. This is why Motion Master is GPL v3
- Commercial license: removes the GPL constraint entirely. With it, Motion Master could use a permissive license such as Apache 2.0
- Breaking change from v1: legacy `ec_` API removed, must use `ecx_` API
- Windows is supported (requires WinPcap)
- IgH EtherCAT Master is Linux-only, so **SOEM is the only open source option for Windows EtherCAT support**

### Current pin

Motion Master builds SOEM from the overlay port in `ports/soem`, not from the stock vcpkg port. The overlay port pins SOEM 2.0.0 at commit `304d1c05eab77dc0d426f1a5cf09c8cc7dc03713` and declares the license `GPL-3.0-only`. The installed copyright file carries the GPL v3 / commercial dual license.

---

## IgH EtherCAT Master (v1.6)

- **Kernel module: GPL v2 or later** — the "or later" makes it GPL v3-compatible, unlike the Linux kernel itself which is v2-only
- **Userspace library (`libethercat`): LGPL v2.1**
  - Dynamic linking: no copyleft propagation. Motion Master could use Apache 2.0 or MIT
  - Static linking: must allow users to relink with a modified version of the library, but your application code stays closed
- **Linux only** — requires a patched Ethernet driver and a kernel module; not usable on Windows

---

## License Compatibility Matrix

| Stack | Open source Motion Master | Commercial Motion Master |
| --- | --- | --- |
| SOEM v1 | Allowed by the linking exception. The SOEM source, including any changes, must be made available under GPL v2 section 3 | Allowed by the linking exception. The SOEM source, including any changes, must be made available under GPL v2 section 3 |
| SOEM v2, GPL v3 path | Must license Motion Master as **GPL v3** ← current | Buy rt-labs commercial license |
| SOEM v2, commercial license | Any permissive license (Apache 2.0, MIT) | Any |
| IgH v1.6, dynamic link | **Apache 2.0 or MIT** viable | Apache 2.0 viable |
| Own driver (see below) | Any license | Any |

---

## EtherCAT Master License (Beckhoff / ETG)

Any EtherCAT master implementation — regardless of software license or who wrote it — requires an **EtherCAT Master License** from the EtherCAT Technology Group (ETG).

- **Free of charge** — it is a compatibility agreement, not a royalty or fee
- **No per-unit royalties**; no special hardware required
- Requirements: sign the agreement; implementation must remain compatible with EtherCAT specifications
- Grants legal certainty around the EtherCAT trademark (owned by Beckhoff Automation GmbH)

---

## Path to a Fully Permissive Commercial License

If a custom EtherCAT master driver is developed in the future, the GPL constraint from SOEM disappears entirely:

1. Write the driver from scratch — must not copy any SOEM or IgH source code (both GPL)
2. Sign the free ETG EtherCAT Master License agreement
3. Implement as a new `FieldbusDriver` concrete class (the architecture already supports this)
4. Remove SOEM from `vcpkg.json`
5. Motion Master can then use any license, including a permissive one such as Apache 2.0, with no GPL obligation and no third-party license fee

---

## Summary

| Goal | Recommended path |
| --- | --- |
| Open source, Windows + Linux | SOEM v2 + **GPL v3** |
| Commercial, Windows + Linux | SOEM v2 + **rt-labs commercial license** |
| Open source, Linux only | IgH dynamic link + **Apache 2.0 or MIT** |
| Full freedom, long term | Own EtherCAT driver + sign free ETG agreement |
