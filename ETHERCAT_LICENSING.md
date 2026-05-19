# EtherCAT Stack Licensing

Motion Master is licensed under **GPL v3** (changed from Apache 2.0 to be compatible with SOEM v2).

## SOEM

### v1 (up to v1.4.0) — GPL v2 only

- **GPL v2 only** (not "or later")
- Has a header/template exception: linking against headers, macros, or inline functions does not trigger GPL, but compiling SOEM source into your binary does
- **Incompatible with Apache 2.0** — the FSF explicitly states Apache 2.0 and GPL v2 cannot be combined for distribution (Apache 2.0 patent retaliation clause conflicts with GPL v2)
- Required an EtherCAT Master License from Beckhoff (see below)

### v2 (v2.0.0+) — GPL v3 or commercial

- **Dual-licensed: GPL v3 OR commercial** (rt-labs AB, contact: sales@rt-labs.com)
- GPL v3 + Apache 2.0: compatible, but the combined work must be distributed as GPL v3 — Motion Master cannot remain Apache 2.0
- Commercial license: removes the GPL constraint entirely — Motion Master can stay Apache 2.0
- Breaking change from v1: legacy `ec_` API removed, must use `ecx_` API
- Windows is supported (requires WinPcap)
- IgH EtherCAT Master is Linux-only, so **SOEM is the only open source option for Windows EtherCAT support**

### Current vcpkg pin

Commit `a901500618405760a564e64a6816705e29f50f9f`, version-date `2023-06-09`. Verify whether this commit falls before or after the v2 dual-licensing change before assuming which license applies.

---

## IgH EtherCAT Master (v1.6)

- **Kernel module: GPL v2 or later** — the "or later" makes it GPL v3-compatible, unlike the Linux kernel itself which is v2-only
- **Userspace library (`libethercat`): LGPL v2.1**
  - Dynamic linking: no copyleft propagation — Motion Master can stay Apache 2.0 or MIT
  - Static linking: must allow users to relink with a modified version of the library, but your application code stays closed
- **Linux only** — requires a patched Ethernet driver and a kernel module; not usable on Windows

---

## License Compatibility Matrix

| Stack | Open source Motion Master | Commercial Motion Master |
|---|---|---|
| SOEM v1 | Not viable (Apache 2.0 + GPL v2 incompatible) | Legally grey |
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
3. Implement as a new `IFieldbusDriver` concrete class (the architecture already supports this)
4. Remove SOEM from `vcpkg.json`
5. Motion Master reverts to pure Apache 2.0 with no GPL encumbrance and no third-party license fees

---

## Summary

| Goal | Recommended path |
|---|---|
| Open source, Windows + Linux | SOEM v2 + **GPL v3** |
| Commercial, Windows + Linux | SOEM v2 + **rt-labs commercial license** |
| Open source, Linux only | IgH dynamic link + **Apache 2.0 or MIT** |
| Full freedom, long term | Own EtherCAT driver + sign free ETG agreement |
