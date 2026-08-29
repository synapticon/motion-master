# Motion Master Features

Motion Master v6 is motion-control software for SOMANET servo drives on EtherCAT. It runs as a local daemon. It serves an HTTP API on `127.0.0.1:61447` and a WebSocket on `127.0.0.1:62281`, and a companion Progressive Web App console drives both. This document catalogs what it does today.

The stable, built-in HTTP API is specified in `apps/motion_master/swagger.yml`, and the running binary serves that same document at `GET /api/swagger.yml`. Installation is covered in [`docs/INSTALLATION.md`](docs/INSTALLATION.md). Configuration and client usage are covered in [`README.md`](./README.md). Architecture and design rationale live in `NEXTGEN.md`.

## The terms used below

EtherCAT calls each device on the bus a **slave**, and its place in the wiring order its **slave position**. Motion Master addresses a device by that position throughout.

**CoE** is CANopen over EtherCAT. It carries an **object dictionary**: a table of a device's parameters, each addressed by an index and a subindex. An **SDO** is a single read or write of one entry, sent through the device's mailbox and answered by the device. A **PDO** is an entry mapped into the cyclic frame instead, so it travels every cycle with no request of its own. **FoE** is File over EtherCAT, which moves whole files.

Each device also holds an **application layer state**, written **AL state**, which decides how much of it is live: INIT, PRE-OP, SAFE-OP, OP, or BOOT. Mailbox traffic works from PRE-OP up. Process data is exchanged in SAFE-OP and OP.

The **ESC** is the EtherCAT Slave Controller, the chip in each device that talks to the wire. The **process image** is the block of memory holding one cycle's inputs and outputs for the whole bus. The **working counter**, written **WKC**, is the number EtherCAT increments as a frame passes each device it addressed, so a short count means a device did not answer.

## Fieldbus and EtherCAT bus control

- **Driver lifecycle** — bring the fieldbus driver up on a chosen network adapter (`POST /api/init`), scan the bus for devices (`POST /api/scan`), and clear the device list again (`POST /api/reset`). The driver sits behind a `FieldbusDriver` interface, so the transport can be replaced. Today that transport is SOEM on a raw socket. SPoE is planned.
- **Network adapter discovery** — list the host's network adapters, so you can pick the one wired to the bus (`GET /api/adapters`).
- **AL state control** — read the current AL state of every device and command a new one (`GET`/`POST /api/devices/state`). A transition the standard does not allow is rejected before it reaches the bus. Entering a state that exchanges process data maps the process image, or re-maps it, as needed.
- **Partial-bus operations** — one or more devices can go to BOOT, where firmware is installed over FoE, or to PRE-OP, where PDO mapping can be rewritten, while the rest keep exchanging process data. Bringing one back re-maps the whole bus, which pauses every device for the duration.
- **Static bus configuration** — read what the master programmed into each device, and what the device says about itself (`GET /api/bus-config`): its identity (vendor, product, revision and serial number), its Sync Manager and Fieldbus Memory Management Unit (FMMU) settings, its distributed-clock configuration, its mailbox windows, and the capability bytes naming which mailbox protocols it claims to support: CoE, FoE, EoE and SoE. The console shows this on its Configuration page and decodes those bytes into flags. Its SII page decodes the same bytes from the raw EEPROM image instead.
- **Live ESC diagnostics** — read each device's ESC error counters, the link state of each of its ports, and how often its watchdog has expired (`GET /api/devices/diagnostics`).
- **Distributed-clock sync status** — the first device with a distributed clock becomes the reference, and every other one corrects its own clock toward it. `GET /api/dc-sync` reports the live deviation of each, in nanoseconds. The figure means something only while the bus is exchanging process data, because that is what carries the reference time, and it settles toward zero. A device whose deviation stays large or grows is the one to look at.

## Process data exchange

- **Real-time cyclic exchange** — a real-time loop exchanges process data every cycle. It runs under `SCHED_FIFO`, with `mlockall` holding its memory in RAM, and the process-data path takes no lock. The default period is 1 ms on Linux and macOS. The Windows release ships 4 ms, because stock Windows timers cannot hold 1 ms. When a deadline has already passed the loop skips to the next point on the grid rather than sending a burst of frames the drives can no longer use, so it slows down predictably instead of drifting.
- **Loop health and retiming** — `GET /api/game-loop` returns a live snapshot: the configured and the achieved rate, how many cycles ran and how many were skipped, the time each task took, and whether the loop got real-time scheduling, locked its memory, and pinned itself to the core it asked for. `PUT /api/game-loop` changes the period while the loop runs. The change takes effect within one cycle and starts a fresh health epoch. It is not written back to the config file. The recorder holds a fixed number of cycles rather than a fixed duration, so a period change resizes nothing.
- **Core pinning** — `gameLoop.cpuAffinity` pins the real-time thread to one core, and only that thread. The HTTP, WebSocket, monitoring and parameter-refresher threads stay on the remaining cores. Point it at a core the kernel booted with `isolcpus`. Such a core is taken out of the scheduler and runs nothing until a thread asks for it by name, so it is wasted until this is set. Linux only. The default, `-1`, leaves the thread unpinned.
- **Process image inspection** — `GET /api/process-image` returns the whole-bus image: the byte size of each direction, the expected and the last working counter, how many layouts have been mapped since the last reset, and every mapped object resolved to its absolute bit offset. When no image is live but one has been mapped before, it returns the last-known layout instead, so a bus that has dropped out of OP still describes itself.
- **Output staging** — write a batch of output values at once (`POST /api/process-data/outputs`). Each value is coerced to its object's declared data type, and an object that is output-mapped on a device that is exchanging is written into the output image, sent on the next cycle, and re-sent every cycle after that. The request never fails as a whole. Each object carries its own result, so a client can see which values are not being sent cyclically.
- **Lossless recorder** — a circular ring records every cycle: the raw input and output images, a timestamp in nanoseconds since the Unix epoch, and the working counter. Its size is a number of cycles set in the config, and the default of 300000 is about five minutes at 1 ms. It is the one source for live monitoring, for reading the freshest value of a single object, and for dumps.
- **Recorder dump** — stream the ring to the client, or write it to a file on the server, as a binary `.mmpd` file (`GET`/`POST /api/process-data/dump`). It works in any state, OP included.
- **PDO mapping** — read which objects a device exchanges cyclically, grouped by object, and rewrite that mapping over CoE (`GET`/`PUT /api/devices/{slavePosition}/pdo-mapping`). A write reconfigures the assignment objects `0x1C12` and `0x1C13` and the mapping objects `0x16xx` and `0x1Axx`, and needs the device in PRE-OP.
- **Process-data watchdog** — read and set the Sync Manager watchdog timeout of one device (`GET`/`PUT /api/devices/{slavePosition}/watchdog`), decoded from its `0x0400` and `0x0420` ESC registers. A device in OP that goes without process data for longer than this faults itself to SAFE-OP with an error. Raising it lets a device sit through the brief pause of a whole-bus re-map. This is the configured timeout, not the expiration count that `GET /api/devices/diagnostics` reports.

## Object dictionary and parameters

- **Parameter enumeration** — build a device's parameter list by walking its object dictionary (`POST /api/devices/{slavePosition}/parameters/init`). With `?readValues=true` the values are read too. An object with several subindices, an ARRAY or a RECORD, is read in one CoE Complete Access upload rather than one upload per subindex, which cuts the number of mailbox round-trips sharply. A device that refuses Complete Access falls back to per-subindex reads without the caller noticing.
- **Reading parameters** — refresh the cached value of every parameter (`POST .../parameters/read`), read the cached list back (`GET .../parameters`), or read one parameter (`GET .../parameters/{index}/{subindex}`), optionally from the cache alone. A read knows where the value lives: a PDO-mapped object comes from the live process image, and everything else over the mailbox, again batching each multi-subindex object into one Complete Access upload where the device allows it.
- **Writing parameters** — write one parameter (`PUT .../parameters/{index}/{subindex}`). This knows where the value belongs the same way a read does.
- **Raw SDO access** — read or write one object dictionary entry directly over CoE, with no caching and no interpretation (`GET`/`PUT /api/devices/{slavePosition}/sdo/{index}/{subindex}`).
- **On-disk parameter cache** — enumerating a dictionary is slow, so the definitions are cached on disk, keyed by the device's vendor, product and revision. List the cache files, download one, or delete one (`GET /api/parameter-cache`, `GET`/`DELETE /api/parameter-cache/{id}`).
- **User cache** — the per-user directory the server writes to, which is also where the rotating log file goes. List every file in it, flattened to one entry per file whatever the nesting, and read one back (`GET /api/user-cache`, `GET /api/user-cache/{path}`). It works whether or not a driver is up.

## Device services

- **File transfer** — read and write a file on a device over FoE, which is how firmware and configuration files move (`GET`/`PUT /api/devices/{slavePosition}/files/{filename}`). List what a SOMANET drive holds, with the size of each file (`GET /api/devices/{slavePosition}/files`). EtherCAT has no directory service, so that listing is the vendor's own `fs-getlist` pseudo-file, which the server reads and parses so each client does not have to.
- **High resolution data** — read back what the *HRD streaming* procedure recorded and turn it into samples (`GET /api/devices/{slavePosition}/hrd?data=…`). The server concatenates the drive's files in order and decodes them as one of two things: the encoder's raw position word, split into its master and nonius tracks, or velocity in RPM converted out of the drive's Q15 fixed point, paired with torque in per mille of rated torque. It answers JSON or CSV, whichever the `Accept` header asks for.
- **ESC register access** — read and write the raw bytes of an ESC register (`GET`/`POST /api/devices/{slavePosition}/registers/{address}`).
- **Parameter persistence** — saving parameters to non-volatile memory (`0x1010`) and restoring a group of defaults (`0x1011`) are procedures rather than requests of their own. Both write a CANopen signature and then poll the device until it confirms, which takes seconds. See *Procedures* below.
- **SII and EEPROM** — read a device's SII image, which is the identity and configuration data held in its EEPROM, and write a raw image back (`GET`/`PUT /api/devices/{slavePosition}/sii`). `POST /api/sii/parse` decodes a raw image with no device present.
- **Hardware identity** — read the device's `.hardware_description` file and, on an Integro, its `.variant` file (`GET /api/devices/{slavePosition}/hardware-description`, `GET /api/devices/{slavePosition}/variant`).
- **Firmware compatibility** — ask whether a firmware package belongs on a device (`GET /api/devices/{slavePosition}/firmware-compatibility?filename=…`). The server assembles the descriptors the device accepts from those two files, and compares them against the descriptor in the package name. A mismatch answers 200 with `compatible: false` and names both descriptors, so a client can say which hardware the package was for. A 4xx means the question could not be asked, because the filename is not a package name or the hardware description could not be read.
- **Device listing** — list every device, or fetch one by bus position (`GET /api/devices`, `GET /api/devices/{slavePosition}`).

## Motion profiles

CiA402 is the CANopen device profile for drives. It defines a state machine that decides whether a drive will produce torque, a set of operation modes, and the objects that drive both. A drive reports its state in the statusword (`0x6041`) and is commanded through the controlword (`0x6040`).

- **Drive snapshot** — `GET /api/devices/{slavePosition}/cia402` returns the decoded state machine state, the raw statusword and controlword, and the active operation mode (`0x6061`). The values are read live, from the process image while the device is exchanging and over SDO otherwise, so a client can poll this to drive a live display.
- **State machine commands** — `POST /api/devices/{slavePosition}/cia402/command` runs one of four: `enable`, which walks every intermediate transition to Operation Enabled and clears a latched fault first if it has to; `disable`, which returns the drive to Switch On Disabled; `quickStop`; and `faultReset`.
- **Named target state** — `POST /api/devices/{slavePosition}/cia402/state` walks the state machine to the state you name, issuing whatever intermediate transitions that takes.
- **Operation mode** — set the mode of operation (`0x6060`) with `POST /api/devices/{slavePosition}/cia402/mode`. The drive reports the mode it accepted in `0x6061` once it takes effect. `GET /api/devices/{slavePosition}/operation-modes` lists every mode that device has, standard and manufacturer-specific, so a client renders the choices it finds instead of holding a list of its own.
- **Setpoints** — `POST /api/devices/{slavePosition}/cia402/target` writes the one setpoint the active mode uses: target position (`0x607A`) in Profile Position and Cyclic Synchronous Position, target velocity (`0x60FF`) in Profile Velocity and Cyclic Synchronous Velocity, or target torque (`0x6071`, in per mille of rated torque) in Profile Torque and Cyclic Synchronous Torque. All three are signed, so a negative value commands reverse motion, or regenerative torque.
- **Brake control** — `GET /api/devices/{slavePosition}/brake` reports the `0x2004` objects that decide what a release or an engage will do, along with the current state. `POST /api/devices/{slavePosition}/brake/release` and `POST .../brake/engage` then do it. Each writes the brake state object (`0x2004:07`) and waits before answering with the state read back: the drive's own pull time on a release, and the `settle` you pass on an engage. Two things to know. A release only happens while the drive is in Operation Enabled; in any other state the write only energises the brake output. And on a pin brake a release moves the shaft, which the `GET` reports as `releaseMovesShaft`.
- **Profile views** — inside the server, a chain of borrowed views, `ProfileDevice ← Cia402Drive ← SomanetDrive`, binds one `Device` for one operation through a factory that validates first. The chain is what provides the state machine and the SOMANET-specific dictionary access behind the endpoints above.
- **Setpoint playback** *(planned)* — `SetpointCyclicTask` will play a precomputed buffer, one setpoint per axis per cycle, in Cyclic Synchronous Position, Velocity or Torque. The mode picks the target object: `0x607A`, `0x60FF` or `0x6071`. The real-time side holds a cursor and nothing else, because the launch path does the operation-mode and enable handshake off the real-time thread before it arms the run. It arms it into a mailbox one deep, where a newer intent replaces an older one and nothing queues, with one slot per axis, so a single-axis move and a coordinated multi-axis program use the same mechanism. The waveform maths lives in pure functions, which the API exposes twice: as a preview endpoint, and as a `{generator, params}` pair a launch request can send in place of an explicit `points` array. A `relative` program is offset by the current value once, when it is armed. Skip handling is chosen at launch: `Sequential` advances the cursor by one and preserves the shape, `RealTime` computes the cursor from elapsed cycles and preserves the timing.

## Procedures

A procedure is an operation that takes longer than a request should. It runs on its own thread and reports progress, rather than holding the connection open.

- **One shape for all of them** — `POST`, `GET` and `DELETE` on `/api/devices/{slavePosition}/procedures/{procedureName}` start a run, report how it is going, and cancel it. The run happens on its own thread, so the API stays responsive and the drive keeps cycling. One procedure runs per device at a time. Progress is polled rather than streamed, and each `GET` returns the whole accumulated run, so a step that starts and finishes between two polls is still reported with its result. The result is retained after the run ends.
- **The catalogue is served, not built into clients** — `GET /api/devices/{slavePosition}/procedures` lists what that device supports. Each entry carries its title, its description, its caveats, whether it can move the shaft, whether the drive must be enabled, the steps it reports against, and the parameters it accepts: name, type, default, bounds or choices, and whether it is required. That is enough to render a control and a form for a procedure a client has never heard of. The list is per device, so a procedure is offered only where it applies.
- **Generic CANopen procedures**, offered on any device with a CoE mailbox: **store parameters** (`0x1010`), and **restore default parameters** (`0x1011`, with a `group` parameter choosing all, communication, application or manufacturer).
- **SOMANET procedures**, each of which is one row in a catalogue table:
  - **OS command** — the raw operating-system command interface (`0x1023` and `0x1024`), with the request bytes passed through as given.
  - **Encoder register communication** — read or write one BiSS encoder register.
  - **iC-MU calibration mode** — set how the BiSS service clocks a Circulo's internal encoder: standard, configuration or raw.
  - **HRD streaming** — record either the encoder's raw position word, or the velocity and torque actual values, at one sample per millisecond. The control loop runs faster than that, so the recording is decimated to 1 kHz.
  - **The motor measurements** — open phase detection, pole pair detection, phase resistance, and phase inductance.
  - **Motor phase order detection** and **commutation offset measurement**.
  - **Offset detection** — the whole commissioning sequence in one prepared session.
- **What a procedure leaves behind** — the measurement procedures prepare the drive themselves, putting it into diagnostics mode and Operation Enabled and handling the brake where the command requires it, and they restore everything on every path out, a failure or a cancellation included. The two encoder procedures prepare nothing, because their commands need nothing prepared. **iC-MU calibration mode is the one procedure that leaves the drive changed**, because the mode it sets is a mode the encoder stays in.

## Auto-tuning

- **A separate executable, started as a child** — the tuning calculations are the controller-gain functions, and the fit that turns a recorded measurement into a plant model. They run in an `auto-tuning` executable that Motion Master starts at startup and reaches over loopback. It computes on the numbers sent to it and drives nothing. The measurement it fits is recorded on the drive, by the *system identification* procedure.
- **Optional, and downloaded rather than shipped** — the file is not in the release archives. Every install path fetches it once from a rolling release, and continues without it if that download fails. A machine that commissions nothing can leave it off, or keep it and set `autoTuning.enabled` to `false`.
- **Status** — `GET /api/auto-tuning` tells four cases apart: switched off in the configuration, not installed, installed but would not start, and running. The values are a startup snapshot, and nothing polls the process afterwards, so a call is the honest test.
- **Run a function** — `POST /api/auto-tuning/run` names a function and its inputs, forwards the request to the process, and returns the reply unchanged. A `503` means no process is running.
- **Its own contract** — `GET /api/auto-tuning/swagger.yml` serves the OpenAPI document the process carries, fetched from it on each request, so a client reads the schemas of the copy actually installed.

## Live telemetry

- **Monitorings** — create, list, read and delete a monitoring (`/api/monitorings`, `/api/monitorings/{topic}`). Each one streams a set of parameters you choose, drawn from the process image and from the mailbox alike.
- **Lossless WebSocket stream** — every recorded cycle is shipped over the WebSocket as a positional row, `{"type":"monitoring", ...}`. The `interval` is how often the rows are flushed, between 5 and 2000 ms, not how often values are sampled, so a longer interval ships more rows per message rather than fewer cycles. No cycle is dropped. Throughput is sized for roughly five clients at once. That is a budget, not an enforced limit, and there is no cap on connections.
- **Notifications** — events about the bus and its state are pushed over the same WebSocket as `{"type":"notification", ...}`.

## Diagnostics and reference data

- **Log access** — read recent server log output (`GET /api/log`).
- **Reference tables** — the codes and register maps a client would otherwise hard-code: AL status codes, ESC registers, FoE error codes, mailbox error codes, SDO abort codes, CoE object data types, and the register maps of the internal encoders, which are iC-Haus iC-MU and iC-PVL on a Circulo and Kübler on an Integro (`GET /api/meta/*`). Each has a console page of its own.
- **Offline parsers** — decode a file with no device present: an ESI (`POST /api/esi/parse`), a `.hardware_description` (`POST /api/hardware-description/parse`), an Integro `.variant` (`POST /api/integro-variant/parse`, with the whole option catalogue at `GET /api/integro-variant/options`), and a SOMANET firmware package filename (`GET /api/firmware-package-name`). The ESI parser earns its place. An ESI is the XML file a vendor ships to describe a device. It is the only source for object descriptions, enum option labels, engineering units, and minimum and maximum bounds, because the CoE SDO-Information service that reports an object's metadata reports none of them.
- **System and version information** — the Motion Master version (`GET /api/version`), the configuration it started with (`GET /api/config`), and the host's operating system and hardware (`GET /api/system-info`).
- **A self-describing API** — the server serves its own OpenAPI document at `GET /api/swagger.yml`, so a client resolves the contract from the instance it is talking to instead of pinning a copy that goes stale. The console renders it as browsable API Docs, and the reference Python client resolves every call against it at startup.

## Web console

A React Progressive Web App at `https://motion-master.synapticon.com` provides a user interface for everything above.

- **Connection** — driver initialisation, adapter selection, and the TLS certificate's status.
- **Fieldbus** — an overview, EtherCAT state control, bus configuration, the process image, bus diagnostics, and DC sync.
- **Per device** — EtherCAT State, Parameters, Object Dictionary, PDO Mapping, Registers, SII, FoE and Procedures, plus two that do more than show one endpoint:
  - **Motion** drives the CiA402 state machine, the operation mode and cyclic setpoints, showing target against actual live. It appears only for a CiA402 device in OP, because both are needed to command motion.
  - **HRD** reads a high resolution recording back and plots it, or downloads it as CSV.
- **Data** — a live process-data view, the recorder, and Monitorings, which configures and plots live telemetry.
- **Game Loop** — the health of the real-time loop, and control of its cycle period while it runs.
- **Storage** — Parameter Cache and User Cache, the two views on the per-user cache root.
- **Tools** — Auto-Tuning, ESI, Integro Variant, SII, and Utilities.
- **Server and reference** — Log; Requests, a client-side record of every HTTP request including the failures, which stays available while the connection is failing and is most useful precisely then; the bundled API Docs; and the reference tables: AL Status Codes, ESC Registers, FoE Error Codes, iC-Haus Registers, Kübler Registers, Mailbox Error Codes, Object Data Types and SDO Abort Codes.
- **Every page explains itself** — each one says what the data means, why it matters, and what the caveats are.

## Security and deployment

- **TLS** — both servers speak TLS. The binary ships a Let's Encrypt certificate for `local.motion-master.synapticon.com`, a name that resolves to `127.0.0.1`, so the console reaches a local server with no browser warning and no per-machine setup. Cross-origin requests are allowed from the console's origin only.
- **Certificate self-heal** — an expired certificate blocks the console's requests with no way for the user to click through, so the binary repairs it itself. At startup, if the certificate is missing, expired, or within seven days of expiring, it downloads a fresh pair, checks that it parses, covers every name served, has not expired and matches its key, and installs it atomically. `--update-cert` does the same from the terminal and exits. `--cert-url` and `--key-url` change where it fetches from. Setting `tls.autoUpdate` to `false` switches it off, which is what an air-gapped install wants. `GET /api/cert` reports validity to the user interface, and `POST /api/cert/refresh` is the manual button.
- **Packaging** — the release ships a `.deb`, an `.rpm` and a `.tar.gz` for Linux, a `.zip` for Windows, a `.tar.gz` for macOS on Apple Silicon, and a Docker image. The packages and the image install to `/opt/motion-master/`, and the deb and rpm also symlink `/usr/local/bin/motion-master`. The zip and the tarballs are self-contained and run from wherever they are extracted. The Windows executable is Authenticode code-signed, so Windows names Synapticon as the publisher rather than blocking it.
- **Extensibility** — a C++ route plug-in adds endpoints under `/api/<yourapp>/...` without touching the core server. `libs/example` is the starter to copy.

## Configuration

- **Config file only, and always optional** — every setting lives in one JSONC file, which is JSON with comments allowed. There are no command-line flags for settings. Every key falls back to a built-in default, so the binary runs with no config file at all. The `Config` struct mirrors the file one to one.
- **Two places it is loaded from** — an explicit `--config <path>` always wins. Without one, a `motion-master.jsonc` sitting next to the executable is picked up. There is no system-wide search path. This is what lets the Windows release ship a config that presets a 4 ms cycle.
- **What is configurable** — one block per concern:
  - `server` — the HTTP and WebSocket ports, and the allowed origin.
  - `fieldbus` — the driver and adapter to bring up at startup.
  - `logging` — log verbosity, and the rotating log file.
  - `gameLoop` — the real-time cycle period, and the core pin.
  - `recorder` — the ring's depth, and where dumps go.
  - `parameterCache` — the on-disk cache of parameter definitions.
  - `parameters` — how the object dictionary is read.
  - `autoTuning` — the auto-tuning child process.
  - `tls` — the certificate and key paths, and certificate auto-update.

  [`README.md`](./README.md#configuration) is the annotated block reference. `apps/motion_master/motion-master.example.jsonc` documents every key with its default.
