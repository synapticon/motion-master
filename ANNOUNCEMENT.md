# Announcing Motion Master v6 — Next Generation

**To:** everyone at Synapticon
**Status:** in active development (`6.0.0-alpha.60`) · **Beta: end of 2026**
**Try it now:** <https://motion-master.synapticon.com>

---

Some of you already know: for the past months we have been working hard on the next
generation of Motion Master. It is live and usable today at
<https://motion-master.synapticon.com> — install a release, open the Console in your
browser, and you are talking to your drives.

This is the announcement of what it is, why we did it this way, where we stand, and what
is still ahead.

**One thing up front:** to get v6 to the finish line, our effort on the current tools will
be reduced over the coming months. Please bear with us — the payoff is a platform we can
build on for years instead of one we keep patching.

---

## The short version

Motion Master v6 is a **complete, clean-sheet rewrite** — not an incremental release. It is
a local daemon that speaks **HTTP (61447) and WebSocket (62281)**, driven by a **browser
PWA console**, with a **hard separation between the real-time and non-real-time context**,
**fully documented**, **unit- and hardware-tested**, and **fully open source under GPL v3**.

It runs on **Linux (x86-64 and aarch64), Raspberry Pi, macOS (Apple Silicon), and Windows**.

---

## The goal

Everything below is in service of seven things. This is the yardstick — if a feature doesn't
move one of these, it doesn't go in.

| Goal | What it means in practice |
| --- | --- |
| **Rock-solid** | A clean real-time/non-real-time boundary, no exceptions, no hidden state changes, predictable degradation under load. It does not surprise you and it does not fall over. |
| **Fully tested** | Unit tests, integration tests against a real running server, hardware-in-the-loop tests on real drives, and a jitter benchmark that measures the production timer. We measure; we don't claim. |
| **Ease of use** | A browser console you open and use, every page explaining itself, errors that name the actual cause, and no configuration required to start. |
| **Supports any EtherCAT device** | Not just SOMANET. Standards-first: CoE, FoE, SII, PDO mapping, DC — including mailbox-less couplers and I/O terminals. If it speaks EtherCAT, it belongs on the bus. |
| **The best tool for commissioning SOMANET devices** | Deep SOMANET and CiA402 support on top of that generic base — the fastest path from a box of parts to a moving axis. |
| **The best tool for scripting and testing** | An HTTP API as the *primary* interface, self-describing, with published clients. Anything the Console can do, a script can do — in any language. |
| **Used in production on a real-time operating system** | Not a commissioning-only tool. `SCHED_FIFO`, `mlockall`, absolute-deadline scheduling, PREEMPT_RT provisioning included — a real master for a real machine. |

---

## Why a rewrite, and not another increment

The previous generation carried a decade of accumulated design decisions that no longer match
what we need:

- **A service layer that hid the fieldbus** instead of exposing it. You could ask it to do
  things; you could not see or control what it was doing.
- **Automatic by default, and therefore rigid.** The software decided the sequence for you and
  changed device state behind your back. That is convenient in exactly the one workflow it was
  written for, and an obstacle in every other — and when something failed mid-sequence, the
  hidden steps were precisely the ones you needed to see. v6 is explicit and flexible instead:
  you drive the states, and nothing moves unless you ask for it.
- **No clear separation between the real-time and non-real-time context.** Without that
  boundary you cannot reason about determinism, test it, or make any honest claim about it —
  which rules out using the software as a real master in production.
- **A data flow far more complicated than the problem required**, with no compensating benefit
  to show for the complexity.
- **Protobuf over ZeroMQ, for no reason that survives scrutiny.** A request channel plus
  pub/sub channels carrying Protobuf messages bought us serialization we did not need, and cost
  us a browser client, a human-readable API, and every off-the-shelf HTTP tool. v6 speaks JSON
  over HTTP and a WebSocket — the entire API is inspectable with `curl`.
- **Features that did not actually work** — PDO remapping and CoE Complete Access among them.

A decade of accumulated design, quite literally: the first commit on that lineage was
`e703512e` — *"Add README.rst file"* by Andrija Feher on **19 October 2016** at 08:04 UTC,
three minutes after the repository was created at 08:01 — and it grew to 4,776 commits over
the following nine years. What accumulated was a codebase we could no longer test with
confidence, extend without fear, or reason about as a whole.

Those are not bugs you fix one at a time — they are consequences of the architecture. So we
started from an empty repository, kept the hard-won domain knowledge, and rebuilt the
structure around three principles:

1. **The user is in control.** Nothing changes state behind your back.
2. **The real-time context is sacred.** It is a named, documented, testable boundary.
3. **Everything is an extension point.** The core stays small; features plug in.

Every architectural decision, including the ones we rejected, is written down in
[`NEXTGEN.md`](NEXTGEN.md).

---

## What is different from the previous generation

### You control the fieldbus — it does not control you

- **Manual AL state control.** You decide when a device goes INIT → PRE-OP → SAFE-OP → OP →
  BOOT. Illegal transitions are rejected up front with a reason, not attempted and crashed.
- **Devices stay in memory until you reset.** If a drive powers off or drops off the bus,
  its last known identity, parameters, and diagnostics are still there to inspect. You can
  find out *why* it went away instead of watching it vanish from a list.
- **Partial-bus operations.** A single device can be taken out of OP into BOOT, flashed with
  new firmware, and commanded back to OP **while every other device keeps exchanging process
  data.** No stopping the whole machine to update one axis.
- **PDO remapping works.** Read a device's cyclic mapping grouped by object and rewrite it
  over CoE.
- **CoE Complete Access works.** Multi-subindex objects are read in one upload instead of one
  per subindex — dramatically fewer mailbox round-trips, with automatic per-subindex
  fallback on slaves that don't support it.
- **Faster mailbox communication** overall, and a **on-disk parameter cache** keyed by device
  identity so object-dictionary definitions are not re-enumerated on every connect.

### Errors that tell you what actually happened

This is the quiet feature that will save you the most hours.

- Every failure carries the **real cause**, not a generic message. A CoE read that the device
  refused reports `SDO abort 0x08000000: General error`. A CoE read the device never answered
  reports `no response — mailbox timeout`. Those are *different faults* and v6 says so.
- **Server-measured wire time on every fieldbus operation** (`X-Wire-Us`). The Console shows
  `SDO 700 ms · round-trip 712 ms`, so you can see instantly whether a slow operation is the
  device, the network, or your browser.
- **Built-in reference tables** for AL status codes, SDO abort codes, FoE error codes, mailbox
  error codes, ESC registers, and CoE data types — served by the API and browsable in the UI.
  No more looking up hex codes in a PDF.
- **Log access over the API** (`GET /api/log`) and a Log page in the Console.

### Real-time you can actually verify

- **`SCHED_FIFO` priority 80 + `mlockall` + absolute-deadline sleeping.** Three primitives,
  documented in [`docs/RT_SCHEDULING.md`](docs/RT_SCHEDULING.md).
- **The RT loop never takes a lock.** The PDO path is lock-free; process-data outputs are
  staged through per-object atomic slots that the RT loop composes into the wire image.
  Explained in [`docs/THREADS.md`](docs/THREADS.md).
- **Configurable cycle period, retimable at runtime.** If your system is skipping cycles, you
  raise the period (`PUT /api/game-loop`) and get deterministic behaviour back — no restart.
- **Skip-to-grid overrun policy.** A missed deadline skips to the next grid point instead of
  replaying stale frames. The loop degrades predictably instead of drifting or bursting.
- **Health you can see:** configured vs. achieved rate, executed and skipped cycle counters,
  per-cycle task timing, and whether RT scheduling was actually acquired — on the Game Loop
  page and over the API.
- **A jitter benchmark** (`hil/jitter_bench`) that runs the *same* timer the production loop
  uses, writes CSV, and plots it. We measure our determinism; we don't claim it.
- **PREEMPT_RT provisioning included** — [`rt/`](rt) has the documentation and Ansible
  playbooks that take a fresh Debian 13 install to a fully configured real-time host
  (RT kernel, core isolation, C-states off, `rtprio`/`memlock` limits, NIC IRQ affinity).

This is what makes v6 usable **as a real master in production**, not just as a
commissioning tool.

### Tested, not hoped

- **20 unit test suites** across `libs/core`, `libs/comm`, and `libs/node`. These run on every
  push on **all four platforms** — Linux x64, Linux arm64, macOS arm64, Windows x64 — so a
  change that compiles or behaves differently on one of them is caught immediately.
- **Integration tests against a real running server** (TypeScript/Vitest, Docker-managed) that
  drive the published client library through the HTTP and WebSocket contract end to end.
- **Hardware-in-the-loop tests** on real drives, and a **jitter benchmark** that exercises the
  production cycle timer.
- Plus format, lint, static analysis, and a check that the generated API client never drifts
  from the spec.

To be straight about the scope: the **unit tests and builds** cover four platforms, but
**hardware validation is a Linux and Windows exercise** — those are the two hosts we test
against real drives. macOS and arm64 are built and unit-tested every push, and arm64 is where
the Raspberry Pi appliance is headed, but neither carries the same hardware coverage yet.

### Documented — all of it

- **The HTTP API is specified in OpenAPI** and the **running binary serves its own spec** at
  `GET /api/swagger.yml`. A client resolves the contract from the very instance it is talking
  to, instead of pinning a copy that goes stale.
- **The C++ source is Doxygen-documented** and published at
  <https://motion-master.synapticon.com/docs>.
- **[`FEATURES.md`](FEATURES.md)** catalogs every capability; **[`README.md`](README.md)**
  covers install, configuration, and clients.
- **Every page in the Console explains what it is** — what the data means, why it matters,
  and what the caveats are. v6 doubles as a way to *learn* EtherCAT and CiA402, which matters
  for onboarding and for customers.

### Fully open source — GPL v3

The whole thing, including the Console. We moved from Apache 2.0 to **GPL v3** to be
compatible with SOEM v2; the reasoning and the licensing landscape are in
[`ETHERCAT_LICENSING.md`](ETHERCAT_LICENSING.md).

---

## A platform, extensible in three tiers

This is the part with the longest-term consequences. v6 is not an application with a plug-in
folder bolted on; extension is the shape of the thing.

**Tier 1 — Web applications.** Every app lives under `web/apps/<name>` and deploys to
`/apps/<name>` (e.g. `/apps/position-configurator`). All of them are **PWAs — installable and
usable offline**. They share:
- `@synapticon/ui` — the Synapticon **design system**: theme plus React components, so a new
  app is on-brand from the first commit.
- `@synapticon/motion-master-client` — the published, isomorphic **TypeScript SDK** (generated
  HTTP client + WebSocket connection), versioned in lockstep with the server.

`web/apps/example` is a working starter you copy.

**Tier 2 — Extend the HTTP API in C++.** Route plug-ins register their own paths under
`/api/<yourapp>/...` from a **separate library outside Motion Master core** — no edits to the
core server. A plug-in holds a `DeviceManager&` like any built-in route and may run its own
off-RT background threads. `libs/example` is the copy-me template.

**Tier 3 — Extend the real-time context.** Add a cyclic task and it runs inside the RT loop
with the same guarantees as process-data exchange, receiving per-cycle timing context so a
time-indexed task stays on schedule even across skipped cycles.

Plus **delivery is solved for extensions too**: the same release pipeline that ships the
server ships the apps — code-signed Windows binaries, automatic TLS certificate renewal, and
one lockstep version across the binary, the apps, and the SDK, so `console@X` is known-good
against `binary@X` with no compatibility matrix to reason about.

---

## The API is a first-class citizen

It is not a bridge or a sidecar — the HTTP API and WebSocket **are** Motion Master's
interface, integrated into the server itself. 48 endpoints today.

Which means you can drive drives from **any language, any tool, any AI agent**:

- **TypeScript** — `@synapticon/motion-master-client`, published to npm on every release.
- **Python** — a reference client in [`clients/python`](clients/python) that reads the
  server's own OpenAPI spec at startup and resolves every call from it, with numbered example
  scripts that read as a walkthrough of the full fieldbus lifecycle.
- **curl, Postman, a shell script, an LLM** — it's HTTP.

Secured properly, too: HTTPS/WSS with a real Let's Encrypt certificate, CORS locked to the
Console origin, monthly automated certificate renewal, and a binary that **self-heals its own
certificate** at startup when it is missing, expired, or expiring soon (with an opt-out for
air-gapped installs).

---

## Monitoring and process data, properly exposed

- **Lossless monitoring.** Every recorded cycle is shipped — the `interval` you configure is
  the *flush cadence*, not a sample rate. You do not silently lose cycles between samples.
- **You choose what to watch.** Any mix of PDO- and SDO-sourced parameters across any devices,
  streamed over the WebSocket as compact positional rows with microsecond timestamps.
- **A ring buffer that records every single PDO cycle**, sized by *your* configured capacity —
  five minutes, an hour, whatever the box has RAM for. It is period-independent, so changing
  the cycle period doesn't resize it.
- **Dump it to a file.** `POST /api/process-data/dump` writes a binary `.mmpd` of the recorded
  window — **in any state, including OP** — and the Console's Recorder page opens it. The
  post-mortem trace of what the bus was actually doing when things went wrong.

---

## Multi-fieldbus, one device abstraction

`FieldbusDriver` abstracts the transport. **SOEM raw-socket EtherCAT** is what ships today;
**SPoE** is next, and the interface exists so a device behaves the same regardless of which
network protocol carries it. Above that line there is a **single `Device` abstraction** — one
concept, not the overlapping device types of the previous generation.

Also worth knowing: **third-party slaves work.** Mailbox-less devices (EtherCAT couplers and
I/O terminals such as Beckhoff EK1100/EL2008) are supported by reading their PDO mapping from
the SII/EEPROM when there is no CoE object dictionary to ask.

---

## Where we are right now

**`6.0.0-alpha.60`.** Roughly 780 commits since the initial commit in May. Released on every
tag for all four platforms, with `.deb`/`.rpm`/tarball, Windows `.zip`, macOS tarball, and a
Docker image.

Working today, on real hardware:

| Area | Status |
| --- | --- |
| Fieldbus lifecycle — init, scan, AL state control, partial-bus operations | ✅ |
| Bus configuration, ESC diagnostics, DC sync status, process image | ✅ |
| Object dictionary / SDO, Complete Access, parameter cache | ✅ |
| PDO mapping read **and write** | ✅ |
| FoE firmware install, including one device at a time on a live bus | ✅ |
| ESC registers, SII/EEPROM read + write, offline SII parser | ✅ |
| CiA402 control — state machine, operation mode, cyclic setpoints | ✅ |
| RT game loop, health, runtime retiming, lossless recorder, `.mmpd` dump | ✅ |
| Monitoring over WebSocket | ✅ |
| Console PWA — 29 pages, all documented in-app | ✅ |
| TypeScript SDK + reference Python client | ✅ |
| Linux x64 / Linux arm64 / macOS arm64 / Windows x64 releases | ✅ |

**All of our existing firmware and devices are supported, Jasper included.**

Still ahead before beta:

- **Trajectory playback in the RT context** — a cyclic task playing back a precomputed
  setpoint buffer (sine, chirp, ramp, step as generated buffers), single-axis and coordinated
  multi-axis, designed and specified, not yet in code.
- **Off-RT procedures** — offset/commutation detection, auto-tuning, and firmware install as
  first-class cancellable procedures with progress reporting.
- **Notification bus** — a general server→client event channel.
- **SPoE driver.**
- **Remote/LAN deployment** — running the server on a separate machine (a flashable Raspberry
  Pi appliance) that the browser reaches over the network. The certificate design for this is
  worked out; the image and discovery are not built yet.
- **DC SYNC0 activation** — hardware-synchronised actuation on a PREEMPT_RT host, for hard
  coordinated multi-axis motion.
- **Topology / cabling map** and a **master-side frame/WKC health timeline.**

We report these openly on purpose: the roadmap is in the repository, not in someone's head.

---

## Timeline

| When | What |
| --- | --- |
| Now → end of 2026 | Alpha releases continue; the remaining features above land. Effort on the current tools is reduced. |
| **End of 2026** | **Beta — feature-complete, API frozen.** |
| Q1 2027 | Official v6.0.0 release. |

Until the beta, the HTTP and WebSocket API may break between any two alphas — that is the
point of a pre-release line, and it is why we would rather break it now than carry it for a
decade. The beta is where it freezes; `6.0.0` is where we stand behind it.

---

## Dig deeper

- **Try it:** <https://motion-master.synapticon.com>
- **Features:** [`FEATURES.md`](FEATURES.md) — every capability, endpoint by endpoint
- **Install & configure:** [`README.md`](README.md)
- **C++ reference:** <https://motion-master.synapticon.com/docs>
- **Class diagram:** [`docs/CLASS_DIAGRAM.md`](docs/CLASS_DIAGRAM.md) — class structure,
  ownership, and inheritance
- **RT scheduling primer:** [`docs/RT_SCHEDULING.md`](docs/RT_SCHEDULING.md) — `SCHED_FIFO`,
  `mlockall`, and absolute-deadline sleeping: the three primitives the cycle depends on
- **Threading model:** [`docs/THREADS.md`](docs/THREADS.md) — the built-in threads, the RT
  cycle, and why the RT loop never takes a lock
- **Design rationale:** [`NEXTGEN.md`](NEXTGEN.md) — every decision and every rejected
  alternative
- **Changelog:** [`CHANGELOG.md`](CHANGELOG.md)

---

## What we need from you

- **Use it.** Install a release, point it at a bus, break it. Bug reports against an alpha are
  worth more than bug reports against a release.
- **Tell us what is missing** for your workflow — there is still time to shape the API before
  it freezes.
- **Be patient with the current tools.** Fewer hands on them for the next few months is the
  cost of getting v6 right.

Questions, ideas, and complaints all welcome.

— The Motion Master team
