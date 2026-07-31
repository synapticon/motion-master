#pragma once

#include <chrono>
#include <expected>
#include <string>
#include <vector>

namespace mm {

/// @brief Number of days before @c notAfter at which a certificate is considered
///        "expiring soon". A cert inside this window is refreshed proactively: it is one of
///        the conditions that trigger the startup self-heal fetch (@c healCertIfNeeded), so
///        a cert is renewed before it lapses rather than after. Also logs a startup warning
///        and sets the @c expiresSoon flag returned by @c GET /api/cert.
inline constexpr int kCertExpiringSoonDays = 7;

/// @brief One certificate in the served chain — its subject/issuer common names plus the issuing
///        organization (the friendly CA name, e.g. "Let's Encrypt", which lives in the O field
///        rather than the CN).
struct CertChainLink {
  std::string subject;       ///< Subject common name (CN).
  std::string issuer;        ///< Issuer common name (CN) — the subject of the next link up.
  std::string organization;  ///< Subject organization (O), e.g. "Let's Encrypt"; "" if absent.
  std::string issuerOrganization;  ///< Issuer organization (O) — names the next link's/root's org.
};

/// @brief Validity window and identity of a TLS leaf certificate, plus the full served chain.
struct CertInfo {
  std::string subject;  ///< Leaf subject common name (CN), e.g. "local.motion-master...".
  std::string issuer;   ///< Leaf issuer common name (CN) — the immediate (intermediate) CA.
  /// The leaf's subjectAltName dNSName entries — the hostnames the certificate is actually valid
  /// for, and what a browser checks (the CN is legacy and ignored). The bundled certificate lists
  /// both the loopback name and the LAN wildcard, so this is what tells a user whether the host
  /// they are typing is covered.
  std::vector<std::string> dnsNames;
  std::chrono::system_clock::time_point notBefore;  ///< Start of the leaf's validity window.
  std::chrono::system_clock::time_point notAfter;   ///< End of the leaf's validity window (expiry).
  /// Every certificate present in the PEM file, leaf first, in chain order (leaf →
  /// intermediate(s)). A fullchain PEM usually stops at the intermediate; the root is in the OS
  /// trust store and not transmitted, but the last link's @c issuer still names it.
  std::vector<CertChainLink> chain;
};

/// @brief Parses a PEM file and extracts the leaf's validity window and the full certificate chain.
///
/// The leaf-level @c subject / @c issuer / @c notBefore / @c notAfter come from the first
/// certificate in the file; @c chain contains every certificate present, leaf first.
///
/// @param certPath  Filesystem path to a PEM-encoded certificate (leaf or fullchain).
/// @return The parsed @c CertInfo, or an error string if the file cannot be opened or parsed.
std::expected<CertInfo, std::string> readCertInfo(const std::string& certPath);

}  // namespace mm
