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
  uint16_t httpPort = 61447;
  uint16_t wsPort = 62281;
  std::string corsOrigin = "https://motion-master.synapticon.com";
};

/// @brief @c "fieldbus" block — the driver to auto-init at startup.
/// An empty @c driver means "do not auto-init"; the fieldbus then waits for @c POST @c /api/init.
struct FieldbusConfig {
  std::string driver;   ///< "" | "soem" | "spoe" (spoe planned; only soem is implemented today).
  std::string adapter;  ///< SOEM NIC: MAC or interface name. "" = none.
};

/// @brief @c "tls" block — certificate paths and startup self-heal policy.
struct TlsConfig {
  /// "" = auto-discover: bundled cert, then acme.sh, then the install-dir default — fetched from
  /// the rolling release on startup if missing or expired (see @c autoUpdate).
  std::string certPath;
  std::string keyPath;     ///< "" = auto-discover, paired with @c certPath.
  bool autoUpdate = true;  ///< Fetch a fresh cert when missing/expired (false ⇒ --no-cert-update).
};

/// @brief @c "gameLoop" block — the real-time cyclic loop.
struct GameLoopConfig {
  uint32_t periodUs = 1000;  ///< Cyclic timer period in microseconds (must be > 0). 1000 = 1 ms.
};

/// @brief @c "recorder" block — the lossless process-data recorder ring.
struct RecorderConfig {
  /// Depth of the recorder ring in seconds (must be > 0). The ring is allocated at process-image
  /// configuration to hold this many seconds of cycles at the GameLoop period, so RAM ≈
  /// historySeconds × (1e6 / periodUs) × per-cycle image bytes (~400). 300 s ≈ 120 MB.
  uint32_t historySeconds = 300;
  /// Directory for `.mmpd` recorder dumps written by @c POST @c /api/process-data/dump. Empty
  /// means a @c "motion-master" subdirectory of the OS temporary directory (resolved at dump time,
  /// cross-platform — never a hardcoded @c /tmp). The directory is created on first dump.
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

/// @brief @c "parameters" block — CoE object-dictionary behaviour.
struct ParametersConfig {
  /// Read each device's object dictionary when it first reaches PRE-OP (the earliest AL state with
  /// a live CoE mailbox), so object names and data types are known for recorder dumps, monitoring,
  /// and the Parameters page without a manual read. Cache-backed (see @c parameterCache), so a
  /// device model pays the (one-time) enumeration once. Disable to keep state transitions minimal —
  /// definitions then load lazily when a device's Parameters are opened.
  bool readObjectDictionaryOnPreop = true;
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
  ParametersConfig parameters;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ServerConfig, httpPort, wsPort, corsOrigin)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FieldbusConfig, driver, adapter)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TlsConfig, certPath, keyPath, autoUpdate)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(GameLoopConfig, periodUs)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RecorderConfig, historySeconds, dumpDir)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ParameterCacheConfig, enabled, cacheAllVendors,
                                                directory)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ParametersConfig, readObjectDictionaryOnPreop)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Config, server, fieldbus, logLevel, tls, gameLoop,
                                                recorder, parameterCache, parameters)

/// @brief Deserialises a parsed JSONC document into a @c Config, applying defaults for absent keys.
///
/// Pure: no file I/O, no process exit. Unknown keys are ignored (forward-compatible). nlohmann
/// validates value *types*; this adds the enum checks it cannot (@c logLevel, @c fieldbus.driver).
///
/// @param doc A parsed JSON value (must be an object).
/// @return The populated @c Config, or an error string on a wrong top-level type, a field type
///         mismatch (from nlohmann), or an invalid enum value.
std::expected<Config, std::string> parseConfig(const nlohmann::json& doc);
