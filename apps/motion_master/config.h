#pragma once

#include <cstdint>
#include <expected>
#include <nlohmann/json.hpp>
#include <string>

/// @file
/// The on-disk configuration, modelled as a struct tree that maps 1-1 to the JSONC file.
///
/// Each sub-struct mirrors one JSONC object; nlohmann's @c _WITH_DEFAULT (de)serialization fills
/// any key the file omits from a default-constructed instance, so a partial config "overrides the
/// defaults" with no per-field plumbing. @c parseConfig adds the enum/value checks nlohmann cannot
/// express. This is deliberately separate from @c Options, which also carries CLI-only fields
/// (actions and cert source URLs) that have no place in the file.

/// @brief @c "server" block — the HTTP API and WebSocket listeners.
struct ServerConfig {
  /// Local address both listeners bind to. The default keeps Motion Master reachable only from the
  /// machine it runs on, which is what @c local.motion-master.synapticon.com (an @c A record pinned
  /// to @c 127.0.0.1) is for. Set @c "0.0.0.0" to serve the whole network — the appliance
  /// deployment, where a browser elsewhere on the LAN drives the bus and reaches this host as
  /// @c <dashed-ip>.ip.motion-master.synapticon.com. The bundled certificate covers both names, so
  /// TLS needs no further configuration either way.
  ///
  /// Motion Master has no authentication: anything that can reach the port can enable drives. Bind
  /// off loopback only on a network you trust.
  std::string bindAddress = "127.0.0.1";
  uint16_t httpPort = 61447;
  uint16_t wsPort = 62281;
  std::string corsOrigin = "https://motion-master.synapticon.com";
};

/// @brief @c "fieldbus" block — the driver to auto-init at startup.
/// An empty @c driver means "do not auto-init"; the fieldbus then waits for @c POST @c /api/init.
struct FieldbusConfig {
  std::string driver;   ///< "" | "soem" | "spoe" (spoe planned; only soem is implemented today).
  std::string adapter;  ///< SOEM NIC: MAC or interface name. "" = none.
  /// SOEM only. Keep SOEM 2.0's mailbox-status FMMU active — the extra input FMMU it maps the SM1
  /// mailbox-status register (0x080D) into the cyclic image on every mailbox slave, letting the
  /// master notice a waiting mailbox message without a separate read. Motion Master does not use
  /// that optimisation, and on TI PRU-ICSS ESCs a register-space FMMU inside an LRW is fatal (every
  /// cyclic frame is dropped, SAFE-OP → OP fails). Default false ⇒ the FMMU is deactivated after
  /// mapping. Set true only for hardware that both needs and supports it.
  bool mailboxStatusFmmu = false;
};

/// @brief @c "tls" block — certificate paths and startup self-heal policy.
struct TlsConfig {
  /// "" = auto-discover: bundled cert, then acme.sh, then the install-dir default — fetched from
  /// the rolling release on startup if missing or expired (see @c autoUpdate).
  std::string certPath;
  std::string keyPath;     ///< "" = auto-discover, paired with @c certPath.
  bool autoUpdate = true;  ///< Fetch a fresh cert when missing/expired/expiring (false ⇒ off).
};

/// @brief @c "gameLoop" block — the real-time cyclic loop.
struct GameLoopConfig {
  uint32_t periodUs = 1000;  ///< Cyclic timer period in microseconds (must be > 0). 1000 = 1 ms.
  /// Core to pin the real-time thread to; -1 (the default) leaves it unpinned. Linux only. Set this
  /// to a core the kernel booted with @c isolcpus: such a core is removed from the scheduler and
  /// runs nothing unless a thread asks for it by name, so an isolated core is wasted until this is
  /// set. Only the RT thread moves — the HTTP, WebSocket, monitoring and refresher threads stay on
  /// the housekeeping cores, which is the difference between this and pinning the whole process
  /// with @c taskset or systemd's @c CPUAffinity=. Unlike @c periodUs this is fixed at startup: it
  /// describes the deployment, not something to retune live. Must name a core that exists.
  int cpuAffinity = -1;
};

/// @brief @c "recorder" block — the lossless process-data recorder ring.
struct RecorderConfig {
  /// Depth of the recorder ring in cycles/rows (must be > 0). The ring is allocated at
  /// process-image configuration to hold exactly this many cycles, independent of the GameLoop
  /// period — records carry absolute timestamps, so the depth-in-seconds is simply capacity ×
  /// period. RAM ≈ capacity × per-cycle record bytes. A record is 28 B fixed (20-byte header +
  /// 8-byte publication word) plus the whole-bus IOmap — ~82 B per SOMANET drive (budget ~100 B).
  /// So a single drive at 300000 cycles × ~128 B ≈ 38 MB (≈ 5 min at a 1 ms period). It scales with
  /// drive count.
  uint32_t capacity = 300000;
  /// Directory for `.mmpd` recorder dumps written by @c POST @c /api/process-data/dump. Empty
  /// means a @c "dumps" subdirectory of the user-cache root (so setting @c userCache.directory
  /// moves the dumps with it), which puts them under @c /api/user-cache — listable, downloadable
  /// and deletable from the Console instead of only from a shell on the host. The directory is
  /// created on first dump. Setting an explicit path opts out of that: dumps then land wherever
  /// this points and the API cannot reach them.
  std::string dumpDir;
};

/// @brief @c "parameterCache" block — the on-disk cache of CoE parameter definitions.
///
/// Enumerating a drive's object dictionary over SDO is hundreds of round-trips; the result (the
/// parameter *definitions* — not live values) is identical for every device of the same vendor,
/// product, and revision, so it is cached to disk and reused on a later scan of the same hardware.
/// Only definitions are cached — live values are always read from the device.
struct ParameterCacheConfig {
  bool enabled = true;  ///< Master switch for the whole cache.
  /// false: cache only Synapticon devices (vendor 0x22D2), whose object dictionary is uniquely
  /// determined by product + revision. true: cache every vendor — only safe when a vendor bumps
  /// its revision whenever the dictionary changes (Motion Master cannot verify that for you).
  bool cacheAllVendors = false;
  std::string directory;  ///< "" = a standard per-user cache directory; set to override.
};

/// @brief @c "userCache" block — the user-writable file store served at @c /api/user-cache.
///
/// A plain place to keep files that should outlive a restart. The store itself neither validates
/// nor interprets an upload — putting a file here does not, on its own, make Motion Master do
/// anything with it. That is separate from whether a *feature* reads a given file: several own
/// subtrees under this root and parse their own (`parameters/`, `dumps/`, more later), and those
/// files show up in the listing alongside whatever a client puts there.
struct UserCacheConfig {
  /// "" = Motion Master's standard per-user cache directory (@c mm::core::userCacheDir):
  /// `%LOCALAPPDATA%\motion-master` on Windows, `~/Library/Caches/motion-master` on macOS,
  /// `$XDG_CACHE_HOME/motion-master` (or `~/.cache/motion-master`) on Linux. Set to override —
  /// note that everything under the chosen directory becomes readable, writable and deletable
  /// through the API, so point it somewhere that holds nothing else.
  std::string directory;
};

/// @brief @c "parameters" block — CoE object-dictionary behaviour.
struct ParametersConfig {
  /// Read each device's object dictionary when it first reaches PRE-OP (the earliest AL state with
  /// a live CoE mailbox), so object names and data types are known for recorder dumps, monitoring,
  /// and the Parameters page without a manual read. Cache-backed (see @c parameterCache), so a
  /// device model pays the (one-time) enumeration once. Disable to keep state transitions minimal —
  /// definitions then load lazily when a device's Parameters are opened.
  bool readObjectDictionaryOnPreop = true;
  /// Read multi-subindex objects (ARRAY/RECORD) with a single CoE Complete Access upload instead of
  /// one upload per subindex when reading parameter values, cutting mailbox round-trips on a full
  /// value read. Support is probed once per device; a slave that rejects it transparently falls
  /// back to per-subindex reads, so this is safe to leave enabled. Disable only to force
  /// per-subindex reads on firmware with a broken Complete Access implementation.
  bool useCompleteAccess = true;
};

/// @brief The whole config file. Top-level keys map to these members.
struct Config {
  ServerConfig server;
  FieldbusConfig fieldbus;
  std::string logLevel = "info";  ///< trace | debug | info | warning | error | critical | off.
  TlsConfig tls;
  GameLoopConfig gameLoop;
  RecorderConfig recorder;
  ParameterCacheConfig parameterCache;
  UserCacheConfig userCache;
  ParametersConfig parameters;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ServerConfig, bindAddress, httpPort, wsPort,
                                                corsOrigin)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FieldbusConfig, driver, adapter, mailboxStatusFmmu)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TlsConfig, certPath, keyPath, autoUpdate)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(GameLoopConfig, periodUs, cpuAffinity)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RecorderConfig, capacity, dumpDir)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ParameterCacheConfig, enabled, cacheAllVendors,
                                                directory)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(UserCacheConfig, directory)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ParametersConfig, readObjectDictionaryOnPreop,
                                                useCompleteAccess)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Config, server, fieldbus, logLevel, tls, gameLoop,
                                                recorder, parameterCache, userCache, parameters)

/// @brief Deserialises a parsed JSONC document into a @c Config, applying defaults for absent keys.
///
/// Pure: no file I/O, no process exit. Unknown keys are ignored (forward-compatible). nlohmann
/// validates value *types*; this adds the enum and range checks it cannot (@c logLevel,
/// @c fieldbus.driver; @c gameLoop.periodUs and @c recorder.capacity must be > 0).
///
/// @param doc A parsed JSON value (must be an object).
/// @return The populated @c Config, or an error string on a wrong top-level type, a field type
///         mismatch (from nlohmann), or an invalid enum value.
std::expected<Config, std::string> parseConfig(const nlohmann::json& doc);
