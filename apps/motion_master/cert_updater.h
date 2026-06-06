#pragma once

#include <expected>
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

}  // namespace mm
