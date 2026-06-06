#pragma once

#include <chrono>
#include <expected>
#include <string>

namespace mm {

/// @brief Number of days before @c notAfter at which a certificate is considered
///        "expiring soon" — drives the startup warning and the @c expiresSoon flag
///        returned by @c GET /api/cert-info.
inline constexpr int kCertExpiryWarningDays = 7;

/// @brief Validity window and identity of a TLS leaf certificate.
struct CertInfo {
  std::string subject;  ///< Subject common name (CN), e.g. "local.motion-master.synapticon.com".
  std::string issuer;   ///< Issuer common name (CN).
  std::chrono::system_clock::time_point notBefore;  ///< Start of the validity window.
  std::chrono::system_clock::time_point notAfter;   ///< End of the validity window (expiry).
};

/// @brief Parses the leaf certificate from a PEM file and extracts its validity window
///        and subject/issuer CNs.
///
/// Only the first certificate in the file is read (the leaf in a fullchain PEM).
///
/// @param certPath  Filesystem path to a PEM-encoded certificate.
/// @return The parsed @c CertInfo, or an error string if the file cannot be opened or parsed.
std::expected<CertInfo, std::string> readCertInfo(const std::string& certPath);

}  // namespace mm
