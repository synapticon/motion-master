#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace mm {

/// @brief Default rolling-release URL for the current TLS certificate.
///
/// Published monthly by `.github/workflows/cert-renewal.yml` to a fixed-tag release, so this URL
/// always serves the freshest cert regardless of when the application was last released.
inline std::string defaultCertUrl() {
  return "https://github.com/synapticon/motion-master/releases/download/tls-cert/cert.pem";
}

/// @brief Default rolling-release URL for the current TLS private key. See @c defaultCertUrl.
inline std::string defaultKeyUrl() {
  return "https://github.com/synapticon/motion-master/releases/download/tls-cert/key.pem";
}

/// @brief Downloads a fresh certificate and key and atomically installs them.
///
/// Fetches @p certUrl and @p keyUrl over HTTPS (following redirects), then validates the
/// downloaded pair before touching the live files:
///   - the certificate parses as PEM and its subject CN is @c kCertCommonName,
///   - the certificate is not already expired,
///   - the private key parses and matches the certificate.
/// Only if all checks pass are @p certPath and @p keyPath atomically replaced (write-to-temp
/// in the same directory, then rename); the key is written with 0600 permissions. On any
/// failure the existing files are left untouched.
///
/// @param certPath  Destination path for the certificate (overwritten on success).
/// @param keyPath   Destination path for the private key (overwritten on success).
/// @param certUrl   Source URL for the certificate.
/// @param keyUrl    Source URL for the private key.
/// @return Empty on success, or an error string describing the first failure.
std::expected<void, std::string> fetchAndSwapCert(const std::string& certPath,
                                                  const std::string& keyPath,
                                                  const std::string& certUrl,
                                                  const std::string& keyUrl);

/// @brief The TLS cert/key paths the server should use, plus where they came from.
struct ResolvedCert {
  std::string certPath;  ///< Resolved certificate path (always populated).
  std::string keyPath;   ///< Resolved private-key path (always populated).
  /// Human-readable description of the discovered source (for the caller to log), or empty when
  /// the paths were configured explicitly or fell through to the install-dir default.
  std::string source;
};

/// @brief Resolve the TLS cert/key file paths, choosing by discovery when not configured.
///
/// If @p configCertPath and @p configKeyPath are both set (e.g. from the config file) they are
/// used as-is. Otherwise both are chosen from the first available source:
///   1. @p defaultCertPath / @p defaultKeyPath next to the binary (a release install),
///   2. `~/.acme.sh/local.motion-master.synapticon.com_ecc/` (a local acme.sh install),
///   3. failing both, @p defaultCertPath / @p defaultKeyPath unconditionally — so the result is
///      always populated and the caller's self-heal can fetch into it.
///
/// @param configCertPath  Certificate path from configuration; empty if unset.
/// @param configKeyPath   Private-key path from configuration; empty if unset.
/// @param defaultCertPath Install-dir certificate path (next to the binary).
/// @param defaultKeyPath  Install-dir private-key path (next to the binary).
/// @return The resolved paths and the discovered source (empty @c source when not discovered).
ResolvedCert resolveCertPaths(const std::string& configCertPath, const std::string& configKeyPath,
                              const std::filesystem::path& defaultCertPath,
                              const std::filesystem::path& defaultKeyPath);

/// @brief Assess the served TLS certificate and refresh it if missing or expired (the startup
///        "cert self-heal").
///
/// Inspects @p certPath: a missing cert or an expired one triggers a fetch from @p certUrl /
/// @p keyUrl via @c fetchAndSwapCert; a valid cert is left alone (an imminent expiry only warns).
/// The outcome is logged. When @p autoUpdate is false no fetch is attempted.
///
/// The fatal case is "no certificate that can be served": the cert is missing and either fetching
/// is disabled or the fetch failed. A present-but-expired cert is a degraded success — it still
/// binds TLS (browsers can click through), so it is served with a warning rather than failing.
///
/// @param certPath   Path to the served certificate (overwritten on a successful fetch).
/// @param keyPath    Path to the served private key (overwritten on a successful fetch).
/// @param autoUpdate Whether a missing/expired cert may be fetched.
/// @param certUrl    Source URL for the certificate fetch.
/// @param keyUrl     Source URL for the private-key fetch.
/// @return Empty on success (including the expired-but-served degraded case), or an error string
///         when TLS cannot be served at all.
std::expected<void, std::string> healCertIfNeeded(const std::string& certPath,
                                                  const std::string& keyPath, bool autoUpdate,
                                                  const std::string& certUrl,
                                                  const std::string& keyUrl);

}  // namespace mm
