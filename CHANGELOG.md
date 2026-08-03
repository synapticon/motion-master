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

### Added

- **Procedures: one uniform way to run the operations a device can carry out.** `POST /api/devices/{slavePosition}/procedures/{procedureName}` starts a run and returns immediately, `GET` on the same path reports how it is going, and `DELETE` cancels it. Every procedure has that same shape regardless of how long it takes — one that finishes before your first read simply reports as succeeded — so a client drives all of them with one loop instead of a per-operation dialect. A run happens on its own thread, so the API stays responsive, monitoring keeps streaming and the drive keeps cycling while it works; only one procedure runs on a device at a time, and a second start is refused rather than queued.
- **Progress is read by polling, and polling cannot miss a result.** Each `GET` returns the whole run — every step with its status and whatever it measured — rather than a stream of events, so a step that starts and finishes between two polls is still reported as succeeded, with its value. There is no WebSocket involved. The result is kept after the run ends, so closing a page and coming back shows how the last run went, with the times it started and finished so a ten-minute-old measurement cannot be mistaken for a current one. A procedure that has never run reports the same shape, idle, so there is no empty state to handle separately.
- **`GET /api/devices/{slavePosition}/procedures` lists what a device can do**, each entry carrying what a client needs to present it — title, description, the caveats that apply, whether running it can move the shaft, whether the drive must be enabled first, and the steps it reports against — together with the state of its current or last run. One request renders a whole per-device view, and the list is per device: a procedure is offered only where it applies, so a third-party slave reports an empty list rather than controls that could only ever fail.
- **Open phase detection, the first purpose-built procedure**: `POST /api/devices/{slavePosition}/procedures/open-phase-detection` checks every motor terminal and FET leg for an open circuit and names the offending one — "open terminal B — terminal B of the drive is not connected" — rather than handing back a raw code. It takes no parameters and prepares the drive itself, in three visible steps: put the drive in diagnostics mode and walk it to Operation Enabled, run the check, then restore the operation mode exactly as it was found and return the drive to Switch On Disabled. The restore runs whether the run succeeds, fails or is cancelled, because a procedure that leaves a drive in diagnostics mode has left the machine worse than it found it. **The brake is left exactly as found**: this command does not require it released, so an engaged brake simply keeps the shaft still while the check runs — but a shaft that nothing holds can turn. A detected open phase is reported as a **failed** run: the check completed and found a fault, which is a result to act on. It appears on the Console's Procedures page automatically, with no Console change, because the page renders whatever the server's catalogue reports.
- **Pole pair detection**, as `POST /api/devices/{slavePosition}/procedures/pole-pair-detection`: counts the connected motor's pole pairs and reports the number. Run after open phase detection when commissioning an absolute-encoder axis, and before motor phase order detection and commutation offset measurement. It takes no parameters and prepares the drive itself in four visible steps — diagnostics mode and Operation Enabled, brake released, detect, then everything restored as found, on every path out including a cancellation. **This one does turn the rotor, and it does release the brake** — both because the command requires it, unlike the other measurements — so the shaft must be free, whatever it drives must be safe to move, and any load a brake was holding needs supporting first. Releasing a pin brake turns the motor by design. The count is reported, not stored. A drive that cannot raise the motor phase currents far enough reports the run as failed, which a limited DC-link voltage or a high motor phase impedance can both cause.
- **Phase resistance and phase inductance measurement**, as `POST /api/devices/{slavePosition}/procedures/phase-resistance-measurement` and `.../phase-inductance-measurement`. Each takes no parameters, prepares the drive itself — diagnostics mode, Operation Enabled — measures, and restores the drive exactly as it was found, whether the run succeeds, fails or is cancelled. The result is reported in the unit the drive reports it in, `milliohms` and `microhenries` respectively, rather than scaled to something rounder. Both are measured at the drive's own terminals, so each reading is the winding plus whatever is in series with it — cabling and connectors included. **The value is reported, not stored**: neither command changes anything in the drive's configuration, so putting the number somewhere is your decision. **Neither releases the brake** — these two commands do not require it released, so nothing that a brake was holding is let go, and a shaft steadied by an engaged brake is the better state to measure in; a shaft that nothing holds can still turn. A drive that cannot raise the motor phase currents far enough reports the run as failed rather than handing back a low reading. Both appear on the Console's Procedures page automatically, with no Console change.
- **Brake control on a SOMANET drive**: `GET /api/devices/{slavePosition}/brake` reports the brake — its state, release strategy, pull time and pull/hold voltages (object 0x2004) — and `POST .../brake/release` and `POST .../brake/engage` command it, each answering with the state read back. Release waits the drive's pull time plus an adjustable `settle`, because the firmware blocks motion, and motion-related OS commands, until that window closes; engage waits only the settle, since a brake is spring-engaged and engaging it is the removal of voltage. Three things the response tells you that are easy to get wrong otherwise: a brake whose release strategy is "manual output voltage" is not driven by the firmware at all, so the commands do nothing and `softwareControllable` says so rather than reporting a failure; releasing a **pin brake** turns the motor, since its release raises torque progressively to lift the load off the pin, which `releaseMovesShaft` flags; and the release only truly happens while the drive is in OP ENABLED — elsewhere the write just energises the brake output. Note that nothing re-engages a released brake for you.
- **A Procedures page per device in the Console.** Pick a procedure from the list on the left and its detail opens on the right: what it does, the caveats that apply, whether it can move the shaft or needs the drive enabled, and a Run button. While it runs, the steps fill in with elapsed time beside the button; when it ends, the result stays. The selected procedure is in the URL, so a specific one can be linked to. The page is built entirely from what the server reports, so procedures appear as they are added without a Console release. Each detail also shows the three requests behind it and the unformatted response body — this console doubles as the reference for anyone building a purpose-built UI on the same endpoints.
- **The first procedure is `os-command`, the direct route to a SOMANET drive's whole OS command set** (CANopen 0x1023 / 0x1024). Supply the eight request bytes — byte 0 the command ID, bytes 1-7 its parameters — and it reports the drive's terminal status, its reply payload and any OS error code, named where the code is a general one. Any command the firmware implements can be run this way. Commands that later get a procedure of their own will name and validate their parameters and decode their result; going straight to a command stays a first-class way to work either way. The bytes are passed through unchecked, so an unintended command ID or parameter reaches the drive as written. Cancelling tells the drive to abort the command it is running rather than waiting it out, so a run stops when you ask.

## [6.0.0-alpha.63] - 2026-08-02

### Added

- **EtherCAT Slave Information (ESI) files can now be parsed offline**, via the new `POST /api/esi/parse` endpoint — the ESI counterpart of the existing SII tool, and usable with no hardware connected. Upload a vendor's ESI XML and get back every device it describes, each with its **complete flat object dictionary**: one row per index/subindex with its display name, data type, bit size, default, minimum, maximum, engineering unit, every access and mapping flag, its description, and its enum option labels. Most of that metadata exists nowhere else — a drive's CoE object dictionary can be enumerated over the bus, but the service returns no descriptions, no enum labels, no units and no bounds.
- The flat dictionary is **assembled the way the device actually presents it**: a SOMANET drive keeps its communication objects in the device description and every CiA402 object (0x6040, 0x6060, 0x607A, …) in a plugged-in module, so the entries are merged across both and each one records which dictionary it came from. Where a slot offers mutually exclusive module variants — the Circulo SMM's four FSoE options — all of them are merged and the overlaps reported, since which one is fitted cannot be known offline; pass `?modules=` to model one configuration exactly.
- A malformed or unusual ESI reports what it found rather than refusing the file: a value whose length disagrees with its declared type, an object referencing a type the dictionary never declares, or an overlap between two modules all come back as warnings alongside the entries that parsed fine.
- **A new ESI page under Tools in the Console.** Load a vendor's `.xml` and read each device's assembled object dictionary as a filterable table — address, name, type, width, access, category, PDO mappability, default, minimum, maximum, unit and which dictionary each entry came from. Click a row for its description, enum option labels and raw vendor properties. The **Modules** table lists what the file declares, which devices can take each one, and a tick-box to narrow the merge — leave them all clear and every module is merged, which is the right default when there is no bus to ask.
- **A user cache: files you upload to Motion Master now stay on the machine it runs on**, through the new `/api/user-cache` endpoints and a **User Cache** page under Storage in the Console. Upload a file to any path you like, list what is stored, download it back, and delete it; sub-directories are created and pruned implicitly, so `captures/2026-08/run-1.bin` just works. Uploading does not by itself make Motion Master do anything with a file — nothing is validated or acted on at upload time, and you choose the paths. Features that need a file on the server can then read it from there rather than each growing an upload page of its own; Motion Master's own object-dictionary caches (`parameters/`) and recorder dumps (`dumps/`) already live under the same root, so they are listed here too. The store lives in the platform's standard per-user cache directory (`~/.cache/motion-master` on Linux, `%LOCALAPPDATA%\motion-master` on Windows, `~/Library/Caches/motion-master` on macOS), overridable with the new `userCache.directory` setting, and the page shows the exact location. Motion Master's own parameter caches live under the same root, so they are listed here too.
- An object's description is carried **once, on subindex 0**, rather than repeated onto every one of its subindices — a RECORD member still has its own, an ARRAY element has none. That alone took a four-device file from 18 MB of JSON down to 3 MB, which is why every device's dictionary now arrives in a single request.

### Changed

- **The parameter-cache endpoints moved from `/api/parameter-caches` to `/api/parameter-cache`** (`listParameterCaches` is now `listParameterCacheEntries` in the TypeScript client), and the Console page is now titled **Parameter Cache**. The plural was the odd one out: the class, the config block and the page all name a single store. Plural collection paths stay plural where the resources really are separate entities — `/api/devices`, `/api/monitorings` — while a *store* you address a key into is singular, matching `/api/user-cache/{path}`.
- **Recorder dumps are now kept in the cache directory instead of the OS temp directory**, in a `dumps/` folder under the user-cache root — so `POST /api/process-data/dump` produces a file you can list, download and delete from the Console's **Storage → User Cache** page, rather than one you could only reach by logging into the machine running Motion Master. Temp was the wrong home for two reasons: the platform reaps it on a timer (`systemd-tmpfiles` clears `/tmp` after days), silently deleting dumps someone meant to keep, and nothing outside that machine could ever see them. Setting `userCache.directory` moves the dumps with it; setting `recorder.dumpDir` explicitly still puts them wherever you point it, outside the API's reach. Note that dumps are large and nothing deletes them for you — that is now a job the UI can actually do.
- **The Recorder page lists the dumps saved on the server and opens them straight into the chart.** Take a dump, then plot it with one click: the file is decoded from the server's copy, so it never has to be downloaded to your computer and re-opened, and it works from a browser with no filesystem access to the machine running Motion Master — the appliance case. The list shows size and time, newest first, and can delete a dump in place. The page's explainer also had the ring's depth wrong: it holds a fixed number of **cycles** (300,000 by default), not a fixed 300 seconds, so the window it covers moves with the loop period — about 5 minutes at a 1 ms cycle, but 20 minutes at the 4 ms Windows default.
- **The Console has a new Storage group**, holding Parameter Cache and User Cache. Both are files Motion Master keeps on the machine it runs on, in the same directory, and they were previously split between Data (otherwise live streams off the bus) and Tools (otherwise offline decoders). Data now holds only live bus data and Tools only the ESI/SII decoders.

## [6.0.0-alpha.62] - 2026-08-01

### Added

- **The real-time thread can now be pinned to a dedicated CPU core**, via the new `gameLoop.cpuAffinity` setting (Linux only; `-1`, unpinned, is the default and nothing changes for an ordinary install). Point it at a core the kernel booted with `isolcpus` and the cycle gets that core to itself. This is what makes core isolation worth doing: `isolcpus` removes a core from the scheduler entirely, so nothing runs there unless a thread asks for it by name — until now an isolated core simply sat idle. Only the real-time thread moves; the HTTP, WebSocket and monitoring threads stay on the remaining cores, which is the difference between this and pinning the whole process with `taskset` or systemd's `CPUAffinity=` (both of which crowd every thread onto the isolated core, and stop `nohz_full` from taking effect). `GET /api/game-loop` reports `cpuAffinity` and `cpuPinned` alongside the existing real-time flags, and the Console's **Game Loop** page shows them.
- `hil/jitter_bench` takes a matching `--cpu <n>`, so a latency measurement can be made under the same pinning the server will run with.
- **Ansible provisioning for real-time Linux hosts**, under `rt/`: a throwaway Debian 14 QEMU virtual machine to develop against, and roles that take a stock Debian install to a configured real-time host — PREEMPT_RT kernel, isolated cores, `rtprio`/`memlock` limits, interrupt affinity, and Motion Master itself as a systemd service. One playbook covers both an x86 industrial PC and a Raspberry Pi 5. See `rt/README.md`.

### Changed

- The Console now explains Chrome's **local network access** prompt, which appears on the first connection to a server on your network and must be allowed. It is unrelated to the certificate — since Chrome 142 a page served from a public address must ask before reaching a private one — and denying it makes requests fail silently, looking exactly like a server that is not running. The unreachable-endpoint message now names it as a possible cause.

## [6.0.0-alpha.61] - 2026-07-31

### Added

- **Motion Master can now be run on one machine and driven from the browser of another** — a Raspberry Pi or industrial PC wired to the drives, with the Console open on a laptop across the network. Set `server.bindAddress` to `0.0.0.0` in the config file (it defaults to `127.0.0.1`, so nothing changes for an ordinary desktop install), then enter that machine's IP address in the Console's **Connection** page. Because no certificate can be issued for a bare IP address, the page offers the equivalent hostname that the bundled certificate *does* cover, behind a **Use hostname** button — take it and the connection is ordinary trusted HTTPS with nothing to install, at the cost of one hosts-file line on the client (which the page spells out, with a button to copy it, and which `add-host.sh` / `add-host.ps1` in the repository will write for you); or keep the plain IP address and grant the browser a certificate exception instead. See `docs/LAN_DEPLOYMENT.md`. **Motion Master has no authentication — anything that can reach these ports can command motion — so only bind off loopback on a network you trust.**
- The **Connection** page now lists the names the served certificate is valid for, and `GET /api/cert` reports them as `dnsNames`. This is what a browser actually checks the host against, so it answers directly whether the address you are connecting to is covered.
- Running on a remote machine involves two manual steps, both documented in `docs/LAN_DEPLOYMENT.md`: one hosts-file line per client machine, and keeping `cert.pem`/`key.pem` current on the server — automatic wherever it has internet access, a periodic copy where it does not. There is no alternative to that copy for an air-gapped machine: no certificate authority, public or private, can issue a certificate valid for more than about two years.

### Changed

- The bundled TLS certificate now covers `*.ip.motion-master.synapticon.com` in addition to `local.motion-master.synapticon.com`, and the monthly renewal issues both names as one certificate. Existing localhost installs are unaffected — same file, same name, same automatic refresh.

## [6.0.0-alpha.60] - 2026-07-28

### Added

- Every release now ships **Linux aarch64 (arm64)** packages alongside the existing x86-64 ones: `motion-master-<version>-linux-arm64.tar.gz`, `-arm64.deb`, and `-aarch64.rpm`. They are built on Debian 13 (trixie) and need glibc 2.38 or newer — the same requirement as the x86-64 binaries — so they install and run on Debian 13 and Raspberry Pi OS trixie. Debian 12 (bookworm, glibc 2.36) still needs a source build.

## [6.0.0-alpha.58] - 2026-07-21

### Fixed

- Live process-data monitoring no longer fails when a mailbox-less slave (e.g. Beckhoff EL2008) is on the bus. Its PDO objects lacked a data type (there is no CoE object dictionary to supply one), which made the Process Data page's monitoring request fail with `400 Bad Request` and silently blank the live values for **every** device on that page. Such a slave's objects now take their data type from the SII EEPROM, so monitoring resolves them and streams values for the whole bus again.

### Added

- Parameters that were read from a slave's SII EEPROM rather than its CoE object dictionary (the case for mailbox-less couplers / I/O terminals) are now flagged with an `origin` of `sii`, shown as an **SII** badge on the Parameters page. These objects have no live SDO access — their value comes only from the process image.

## [6.0.0-alpha.57] - 2026-07-21

### Added

- Simple EtherCAT slaves that have no CoE mailbox now work — EtherCAT couplers (e.g. Beckhoff EK1100) and I/O terminals (e.g. Beckhoff EL2008). When a device has no object dictionary to query over CoE, its PDO mapping is read from the SII/EEPROM instead, so a mailbox-less digital-output terminal's channels now appear on the Process Image and Process Data pages and can be driven by writing their RxPDO outputs. A coupler with no process data of its own contributes nothing, as expected.

## [6.0.0-alpha.56] - 2026-07-21

### Added

- Failed device transactions now show their wire time too: a Read/Write SDO or FoE read/write that fails displays the server-measured device wire time next to the browser round-trip, exactly as a successful one does. So a slow failure — e.g. a CoE read that waits out a ~700 ms mailbox timeout — is now visible as `SDO 700 ms · round-trip …` rather than an error with no timing.
- The Console now shows the device-wire-time-vs-browser-round-trip readout on every page that performs a fieldbus operation, not just SDO/FoE: ESC Registers (read/write), SII/EEPROM read, Diagnostics, DC Sync, the process-data Watchdog (write), PDO Mapping (read/write), and the Object Dictionary enumerate / read-all-values actions on the Parameters page. Each reads the `X-Wire-Us` response header and displays e.g. `Diagnostics 1.8 ms · round-trip 12 ms`, with tooltips explaining the split.

### Changed

- The `X-Wire-Us` server-measured timing header is now sent by **every** endpoint that performs a fieldbus operation, not just SDO/FoE — reading the object dictionary (`parameters/init`, `parameters/read`, single `parameters/:index/:subindex` read/write), ESC registers, the SII/EEPROM, ESC diagnostics, DC-sync status, the process-data watchdog, the PDO mapping (read/write), and the CiA402 snapshot. It is emitted on both the success and the failure response. Its meaning is broadened accordingly: for a single-transaction endpoint it is the pure wire round-trip; for a multi-transaction one (e.g. object-dictionary enumeration, which is hundreds of reads over several seconds) it is the total on-device time of the operation.

### Fixed

- A CoE SDO read or write that the device never answers (a mailbox-receive timeout) now reports `… failed (no response — mailbox timeout)` instead of a bare `… failed`. This distinguishes "the device didn't respond at all" from "the device refused the request" (which still shows the specific SDO abort code, e.g. `SDO abort 0x08000000: General error`) — so, for example, reading an undefined object subindex no longer looks like an unexplained failure.

## [6.0.0-alpha.55] - 2026-07-20

### Fixed

- The Console no longer crashes to a blank screen when a previously saved connection endpoint has an invalid port (e.g. a mistyped `622811`). The monitoring WebSocket now fails to open gracefully instead of throwing, and the Connection page rejects an out-of-range port (must be 1–65535) before it can be applied — so a bad value can neither be entered nor brick the app on the next load. To recover an already-broken tab, click **Load defaults** on the Connection page (now reachable again) or clear the site's storage.

## [6.0.0-alpha.54] - 2026-07-20

### Added

- Console FoE page: reading or writing a file now shows how long the transfer took — the server-measured FoE wire time next to the browser round-trip (e.g. `FoE 5.4 ms · round-trip 37 ms`), each with a tooltip — matching the Read/Write SDO readout on the Parameters page.

### Changed

- The server-measured device wire time is now reported through a common `X-Wire-Us` response header (microseconds) on the CoE SDO **and** FoE read/write endpoints, so it works uniformly whether the response body is JSON or a raw file. The previous `wireUs` body field on the SDO read/write responses has been removed (use the header instead).

### Fixed

- Console Parameters page: clicking **Read SDO** or **Write SDO** is now instant, and its round-trip figure is accurate. The click previously re-rendered the entire parameter list, which blocked the browser and inflated the reported round-trip (~60 ms) far above the true wire time (~10 ms); the SDO tools are now isolated so a click re-renders only them.

## [6.0.0-alpha.53] - 2026-07-19

### Added

- Console Parameters page: the **Read SDO** and **Write SDO** tools now show how long each operation took — the server-measured SDO wire transaction next to the browser round-trip (e.g. `SDO 5.0 ms · round-trip 45 ms`), each with a tooltip explaining what it means. This makes clear that the SDO itself is fast and the larger figure is cross-origin/transport overhead, not device time. Backed by a new `wireUs` field on the CoE SDO read/write API responses.

### Changed

- Repeated writes from the web apps are faster: the CORS preflight (`OPTIONS /api/*`) now sends `Access-Control-Max-Age`, so the browser caches it and skips the extra preflight round-trip before each mutating request.

### Fixed

- Console Game Loop page: the cycle-period input is no longer cramped and now matches the height of its Apply button.

## [6.0.0-alpha.52] - 2026-07-19

### Fixed

- Console Connection page: editing the Host or Port fields no longer loses focus after a single character or throws a "Failed to construct 'WebSocket'" error from half-typed values. The endpoint is now edited locally and applied all at once with an **Apply** button (or Enter), so the API client and monitoring WebSocket are rebuilt only when you commit a change — not on every keystroke.

## [6.0.0-alpha.51] - 2026-07-19

### Added

- New per-drive **Motion** page in the Console for any device that implements the CiA402 drive profile. From it you can step the device-control state machine (Enable walks every transition up to Operation Enabled, clearing a fault first if needed; Disable, Quick stop, Reset fault), select the operation mode (0x6060), command the cyclic setpoint for the active mode (target position, velocity, or torque), and watch target vs. actual stream live on a chart. The page appears in the sidebar only for CiA402 drives currently in the operational (OP) state, since a motion command has no effect otherwise. Backed by new HTTP endpoints under `/api/devices/{slavePosition}/cia402` (status, mode, command, target), and a new `isCia402` flag on the device listing so clients can gate the CiA402-only UI.

## [6.0.0-alpha.50] - 2026-07-18

### Changed

- Hardened the EtherCAT master's working-counter checks on per-slave register operations: process-image mapping now applies the exact success criterion for single-slave register access and logs a diagnostic when a device's FMMU reset does not take effect, instead of discarding that result silently.

## [6.0.0-alpha.49] - 2026-07-17

### Added

- Motion Master now auto-loads a `motion-master.jsonc` placed next to the executable — no `--config` needed (an explicit `--config` still overrides it). The Windows release ships such a file preset to a 4 ms real-time cycle period, which stock Windows timers can sustain reliably; Linux and macOS keep the 1 ms default and ship only the annotated `motion-master.example.jsonc`.

## [6.0.0-alpha.48] - 2026-07-17

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

[Unreleased]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.63...HEAD
[6.0.0-alpha.63]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.62...v6.0.0-alpha.63
[6.0.0-alpha.62]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.61...v6.0.0-alpha.62
[6.0.0-alpha.61]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.60...v6.0.0-alpha.61
[6.0.0-alpha.60]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.59...v6.0.0-alpha.60
[6.0.0-alpha.58]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.57...v6.0.0-alpha.58
[6.0.0-alpha.57]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.56...v6.0.0-alpha.57
[6.0.0-alpha.56]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.55...v6.0.0-alpha.56
[6.0.0-alpha.55]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.54...v6.0.0-alpha.55
[6.0.0-alpha.54]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.53...v6.0.0-alpha.54
[6.0.0-alpha.53]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.52...v6.0.0-alpha.53
[6.0.0-alpha.52]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.51...v6.0.0-alpha.52
[6.0.0-alpha.51]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.50...v6.0.0-alpha.51
[6.0.0-alpha.50]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.49...v6.0.0-alpha.50
[6.0.0-alpha.49]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.48...v6.0.0-alpha.49
[6.0.0-alpha.48]: https://github.com/synapticon/motion-master/compare/v6.0.0-alpha.47...v6.0.0-alpha.48
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
