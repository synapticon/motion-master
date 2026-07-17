# Changelog

All notable changes to Motion Master are documented in this file.

Motion Master ships as a single lockstep version across every artifact — the
`motion-master` server binary, the web apps under `web/apps/*` (Console,
Example), and the `@synapticon/motion-master-client` TypeScript library — because
they all depend on one shared contract, the HTTP API (port 61447) and the
WebSocket protocol (port 62281). One version, one timeline, one changelog.

This changelog records notable *user-facing* changes only. Internal design notes
live in `NEXTGEN.md`, and CI/build plumbing, refactors, and formatting live in the
git history.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Note: `6.0.0` is a pre-release line (`-alpha.N`) with no compatibility guarantees —
the HTTP/WebSocket API may break between any two alphas.

## [Unreleased]

### Changed

- Motion Master now refuses to start a second instance on the same machine, exiting with a clear error instead of silently running two masters that would fight over the EtherCAT NIC and share the API/WebSocket ports. A port already held by any other process now fails fast at startup rather than being silently shared.

## [6.0.0-alpha.47] - 2026-07-17

### Added

- `GET /api/swagger.yml` serves the OpenAPI spec directly from the running server, and a spec-driven Python reference client (with runnable examples) ships alongside it.
- Console: FoE read results can now be viewed as JSON.

### Fixed

- Disabled TLS 1.3 session tickets to stop intermittent WebSocket-upgrade hangs.

## [6.0.0-alpha.46] - 2026-07-16

### Changed

- `GET /api/adapters` now returns each network adapter as `{ macLinux, macWindows, name }` instead of `{ mac, name }`, exposing both platform MAC-address forms.
- Unified the logo and wordmark across the landing page and the console sidebar.
- The process-data write callout now uses the warning colour to flag its effect.

### Fixed

- SII page: FMMU defaults now decode correctly — one entry per FMMU, each labelled with its role (Outputs, Inputs, SyncM status) — instead of showing garbled 16-bit values and the wrong count. The General category's Physical Port and Physical Memory Address now show the correct values.
- Network-adapter matching now rejects MAC addresses that mix `:` and `-` separators.

## [6.0.0-alpha.45] - 2026-07-16

### Added

- Meta catalogues for SDO abort codes and mailbox error codes; abort/mailbox codes are decoded into human-readable error text.
- CSV export and per-action tooltips on the Monitorings page.
- FoE read/write transfer time is shown after a transfer.
- ETG.1020 `WSTRING` (0x0268) object data type in the object-data-type catalogue.

### Fixed

- Corrected and completed the ESC / FoE / AL status meta catalogues.

### Changed

- Consistent page/callout spacing and content top padding; help cursor on Game Loop metric labels.

## [6.0.0-alpha.44] - 2026-07-15

### Changed

- Uniform collapsible sidebar groups.

## [6.0.0-alpha.43] - 2026-07-15

### Changed

- Sidebar branding polish — wordmark matched to the landing page, wider sidebar, teal dictionary/mailbox glyphs, high-visibility orange slave-position badge, gradient logo header.

## [6.0.0-alpha.42] - 2026-07-15

### Changed

- The hosted web apps now deploy only on release tags while the docs site keeps deploying continuously, so the hosted app always matches a released binary.

### Fixed

- Fixed asset paths so the apps load correctly when served under `/apps/<name>/`.

## [6.0.0-alpha.41] - 2026-07-15

### Fixed

- Fixed the macOS and Windows builds.

### Changed

- Softened the sidebar logo status halo.

## [6.0.0-alpha.40] - 2026-07-15

### Added

- Extended the AL status code table beyond ETG.1000.6 V1.0.4.

### Changed

- Sidebar branding — logo, centered header, connection-status halo; routes grouped under their sidebar section and pages named after their routes.
- Corrected object data type names and added missing types per ETG.1020 v1.6.0.

### Fixed

- The Control page now lives at `/control` instead of `/fieldbus`.

## [6.0.0-alpha.39] - 2026-07-14

### Fixed

- Non-shifting game-loop skip status; the skip tile only shows against a live rate.

## [6.0.0-alpha.38] - 2026-07-14

### Added

- Runtime cycle-period control (`PUT /api/game-loop`) — retime the live real-time loop without a restart.

### Changed

- The process-data recorder is now period-independent — it stores a fixed number of cycles regardless of the loop period.

## [6.0.0-alpha.37] - 2026-07-14

### Fixed

- Corrected the cyclic-timer deadline on macOS.

## [6.0.0-alpha.36] - 2026-07-14

### Added

- `GET /api/game-loop` real-time health endpoint (executed/skipped cycles, task-time stats) and a Game Loop page in the Console with a teaching panel.
- Skip-to-grid overrun policy for the real-time loop — a stalled cycle fast-forwards to the next grid point instead of replaying stale frames.
- Accept the ESI `#x` hex prefix wherever a hex-or-decimal value is entered.

### Changed

- Sidebar bus/data links are gated behind a scanned bus; API-dependent links are hidden and queries pause while the API is offline; the Connection page shows a purposeful offline state.

### Fixed

- Grant the capability needed to pin real-time memory when installed from a package.
- More robust certificate date parsing and TLS chain handling.
- Correct process-data handling on TI PRU-ICSS ESCs.

### Removed

- The reserved `"pdos"` monitoring topic — create the monitoring you need; there is no built-in whole-image stream.

## [6.0.0-alpha.35] - 2026-07-08

### Added

- The TLS certificate now refreshes at startup when it is expiring soon (not only after it lapses), so fresh and containerized installs self-heal to a real certificate on first run.

### Fixed

- The Console no longer caches API responses in its service worker.

## [6.0.0-alpha.34] - 2026-07-07

### Added

- Faster bulk parameter reads via CoE Complete Access (one transfer per multi-subindex object); advertised CoE/FoE/EoE/SoE mailbox capabilities surfaced and shown on the Configuration and SII pages with device identity.

### Fixed

- Fixed process data on TI PRU-ICSS ESCs and corrected the SII parse.

## [6.0.0-alpha.33] - 2026-07-06

### Changed

- Hints are always shown inline (the hints preference was removed).

## [6.0.0-alpha.32] - 2026-07-06

### Added

- Read and write a device's PDO mapping over CoE (`GET`/`PUT /api/devices/:slavePosition/pdo-mapping`), with a PDO mapping editor page.
- C++ route-plugin extension point — extend the HTTP API without modifying the core server.
- A shared UI package and an Example starter app; self-hosted fonts; the Console is served at `/apps/console`, with a real 404 and deep-link fallback for the apps.

### Fixed

- An empty bus is treated as a successful scan, not an error.
- Certificate date-parse failures are surfaced instead of yielding a 1970 date.
- UI aligned to the design-system palette; the not-yet-implemented SPoE/IgH driver options are disabled.

## [6.0.0-alpha.31] - 2026-06-16

### Added

- The server and web-app version are shown on the Connection page.

### Fixed

- Object-dictionary entry names are no longer truncated.
- Tolerate TwinCAT alignment-padding PDOs when reading the PDO assignment.

## [6.0.0-alpha.30] - 2026-06-16

### Added

- Hex/dec toggle on parameter value inputs; an FoE tag for devices in BOOT.

## [6.0.0-alpha.29] - 2026-06-15

### Changed

- Connection state is derived from the server rather than browser session storage.

## [6.0.0-alpha.28] - 2026-06-15

### Added

- Process Data page with batch output staging; a "Read All Values" action; the sidebar polls device AL state and derives the mailbox icon from it.

## [6.0.0-alpha.27] - 2026-06-15

### Added

- PDO-aware per-parameter read/write — reads prefer the live process image for PDO-mapped objects.

## [6.0.0-alpha.26] - 2026-06-15

### Fixed

- No longer crashes when serializing a non-UTF-8 string parameter value.

## [6.0.0-alpha.25] - 2026-06-15

### Fixed

- The parameter list refreshes after the automatic object-dictionary read.

## [6.0.0-alpha.24] - 2026-06-15

### Added

- macOS setup guide; per-platform setup docs.

## [6.0.0-alpha.23] - 2026-06-15

### Added

- Release archives now include the annotated example config.
- The generated HTTP API client is committed to the repo and kept in sync with the API by a CI drift-check.

## [6.0.0-alpha.22] - 2026-06-15

### Added

- On-demand `.mmpd` recorder dump streamed over HTTP (`GET /api/process-data/dump`) with a Recorder-page viewer and a client-side parser.
- SII (EEPROM) read *and write*, with a device SII page and a hex viewer; a Parameter Caches management page; a host System panel (OS/hardware/disk/Docker) on the Connection page.
- On-disk parameter-definition cache that skips re-enumeration on a warm cache; the object dictionary is read automatically when a device reaches PRE-OP.
- Explainer panels across the Fieldbus pages (Control, Configuration, Process Image, Diagnostics, DC Sync); inline parameter editing; a searchable parameter picker; a per-device AL-state transition dropdown on Control.
- A slave stuck in INIT is kicked with INIT+ACK before a state request.

### Fixed

- Reset the master mailbox counter on a BOOT/PRE-OP re-init; resync a monitoring stranded past a re-mapped recorder head.
- AL transition failures are reported instead of showing a false success.

## [6.0.0-alpha.21] - 2026-06-09

### Added

- Lossless process-data recorder — every real-time cycle is captured.
- Settings are now configured via a JSONC config file (`--config`), including the loop period; the started configuration is shown on the Connection page.
- Cycle-time stats above each monitoring plot; auto-generated names for new monitorings.

### Changed

- Monitoring flush interval widened to 5–2000 ms (default 16 ms); the object dictionary is read on demand when creating a monitoring.

### Fixed

- Monitoring plots use relative elapsed time; a chart resets on resume so the pause gap doesn't skew it; hover readout in microseconds.

## [6.0.0-alpha.20] - 2026-06-08

### Fixed

- Illegal EtherCAT AL state transitions are rejected before the re-map, and per-slave FMMU state is reset on a re-map — together fixing a segfault when re-mapping after a BOOT excursion.

## [6.0.0-alpha.19] - 2026-06-06

### Added

- TLS certificate self-heal — a missing or expired certificate is fetched automatically before the server binds; a certificate viewer and a manual refresh button in the Console.
- CiA402 / SOMANET drive profile support.

### Changed

- The HTTP API and WebSocket now run on separate ports (defaults 61447 / 62281).
- The output process-data path is lock-free with no per-cycle allocation.

## [6.0.0-alpha.18] - 2026-06-04

First tracked release. Motion Master v6 is a clean-sheet rewrite; this alpha
consolidates the initial platform. (Releases were not individually tagged before
this point — see the git history for the pre-alpha.18 commits.)

### Added

- **Real-time loop:** a `SCHED_FIFO` / `mlockall` cyclic real-time loop with a jitter benchmark.
- **EtherCAT (SOEM):** device and network scanning, network-adapter enumeration, AL state control, ESC register read/write and register map, SDO upload/download, object-dictionary enumeration (name/unit/default/min/max), FoE file read/write, and code catalogues for AL status, ESC registers, and FoE errors.
- **Process data:** PDO process-data exchange with reactive mapping, bus diagnostics (error counters / link / watchdog), DC sync, and a configurable watchdog timeout.
- **Parameters:** typed parameter get/set with online/offline cache sync, per-device online status, and module-ident reconcile on PRE-OP.
- **Monitoring:** off-real-time sampling streamed over the WebSocket by topic, with live charts, play/pause, and retention in the Console.
- **API & app:** an HTTP + WebSocket server with a documented OpenAPI spec, an in-memory log endpoint, and an API-driven fieldbus lifecycle (init / scan / reset).
- **Console:** a React PWA with the Synapticon design system — landing page, dashboard, Connection/Fieldbus/Devices, Parameters, Registers, Log, Requests, FoE file management, Monitorings, and an in-app OpenAPI docs page.
- **Distribution:** TLS certificate automation and release workflow; `.deb`/`.rpm` packages; Windows and macOS (Apple Silicon) builds; a Docker image; an `--open` flag to launch the browser; JSONC config format.

### Fixed

- Clean shutdown (Ctrl+C exits even with a client connected); object-dictionary names no longer corrupted; slaves with terminal AL status codes are dropped during a transition; refresh no longer re-scans and resets slaves to INIT.

[Unreleased]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.47...HEAD
[6.0.0-alpha.47]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.46...v6.0.0-alpha.47
[6.0.0-alpha.46]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.45...v6.0.0-alpha.46
[6.0.0-alpha.45]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.44...v6.0.0-alpha.45
[6.0.0-alpha.44]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.43...v6.0.0-alpha.44
[6.0.0-alpha.43]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.42...v6.0.0-alpha.43
[6.0.0-alpha.42]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.41...v6.0.0-alpha.42
[6.0.0-alpha.41]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.40...v6.0.0-alpha.41
[6.0.0-alpha.40]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.39...v6.0.0-alpha.40
[6.0.0-alpha.39]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.38...v6.0.0-alpha.39
[6.0.0-alpha.38]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.37...v6.0.0-alpha.38
[6.0.0-alpha.37]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.36...v6.0.0-alpha.37
[6.0.0-alpha.36]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.35...v6.0.0-alpha.36
[6.0.0-alpha.35]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.34...v6.0.0-alpha.35
[6.0.0-alpha.34]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.33...v6.0.0-alpha.34
[6.0.0-alpha.33]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.32...v6.0.0-alpha.33
[6.0.0-alpha.32]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.31...v6.0.0-alpha.32
[6.0.0-alpha.31]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.30...v6.0.0-alpha.31
[6.0.0-alpha.30]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.29...v6.0.0-alpha.30
[6.0.0-alpha.29]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.28...v6.0.0-alpha.29
[6.0.0-alpha.28]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.27...v6.0.0-alpha.28
[6.0.0-alpha.27]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.26...v6.0.0-alpha.27
[6.0.0-alpha.26]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.25...v6.0.0-alpha.26
[6.0.0-alpha.25]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.24...v6.0.0-alpha.25
[6.0.0-alpha.24]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.23...v6.0.0-alpha.24
[6.0.0-alpha.23]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.22...v6.0.0-alpha.23
[6.0.0-alpha.22]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.21...v6.0.0-alpha.22
[6.0.0-alpha.21]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.20...v6.0.0-alpha.21
[6.0.0-alpha.20]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.19...v6.0.0-alpha.20
[6.0.0-alpha.19]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.18...v6.0.0-alpha.19
[6.0.0-alpha.18]: https://github.com/synapticon/motion-master/releases/tag/v6.0.0-alpha.18
