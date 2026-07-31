#include "cert_info.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstddef>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mm {

namespace {

// Converts a struct tm interpreted as UTC into a time_point. timegm is the inverse of gmtime;
// MSVC spells it _mkgmtime.
std::chrono::system_clock::time_point tmUtcToTimePoint(std::tm& tm) {
#ifdef _WIN32
  const std::time_t t = _mkgmtime(&tm);
#else
  const std::time_t t = timegm(&tm);
#endif
  return std::chrono::system_clock::from_time_t(t);
}

// Reads an ASN1_TIME (a certificate's notBefore/notAfter) into a time_point, or nullopt when the
// field is absent or cannot be parsed. Without this check a parse failure would leave tm zeroed and
// silently yield the epoch (1970), making a valid cert look long expired.
std::optional<std::chrono::system_clock::time_point> asn1TimeToTimePoint(const ASN1_TIME* t) {
  if (t == nullptr) {
    return std::nullopt;
  }
  std::tm tm{};
  if (ASN1_TIME_to_tm(t, &tm) != 1) {
    return std::nullopt;
  }
  return tmUtcToTimePoint(tm);
}

// Extracts a single text entry (by NID) from an X509_NAME, or "" if absent.
std::string nameEntry(X509_NAME* name, int nid) {
  if (name == nullptr) {
    return {};
  }
  char buf[256];
  const int len = X509_NAME_get_text_by_NID(name, nid, buf, sizeof(buf));
  if (len < 0) {
    return {};
  }
  return std::string(buf, static_cast<std::size_t>(len));
}

// Extracts the common name (CN) from an X509_NAME, or "" if absent.
std::string commonName(X509_NAME* name) { return nameEntry(name, NID_commonName); }

// Extracts the organization (O) from an X509_NAME, or "" if absent — the friendly CA name
// (e.g. "Let's Encrypt") that the short CN ("R10", "YE2") does not carry.
std::string organizationName(X509_NAME* name) { return nameEntry(name, NID_organizationName); }

// Extracts the subjectAltName dNSName entries — the hostnames the certificate is valid for. Only
// DNS entries are reported; an IP-address SAN (GEN_IPADD, which the dev self-signed cert carries)
// is skipped, since these names exist to be matched against a hostname. Returns empty when the
// certificate has no SAN extension at all.
std::vector<std::string> dnsNames(X509* cert) {
  std::vector<std::string> names;
  auto* san =
      static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
  if (san == nullptr) {
    return names;
  }
  const int count = sk_GENERAL_NAME_num(san);
  for (int i = 0; i < count; ++i) {
    const GENERAL_NAME* entry = sk_GENERAL_NAME_value(san, i);
    if (entry == nullptr || entry->type != GEN_DNS) {
      continue;
    }
    const unsigned char* data = ASN1_STRING_get0_data(entry->d.dNSName);
    const int len = ASN1_STRING_length(entry->d.dNSName);
    if (data != nullptr && len > 0) {
      names.emplace_back(reinterpret_cast<const char*>(data), static_cast<std::size_t>(len));
    }
  }
  GENERAL_NAMES_free(san);
  return names;
}

}  // namespace

std::expected<CertInfo, std::string> readCertInfo(const std::string& certPath) {
  const std::unique_ptr<BIO, decltype(&BIO_free)> bio{BIO_new_file(certPath.c_str(), "r"),
                                                      BIO_free};
  if (!bio) {
    return std::unexpected("cannot open certificate file: " + certPath);
  }

  const std::unique_ptr<X509, decltype(&X509_free)> leaf{
      PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free};
  if (!leaf) {
    return std::unexpected("cannot parse PEM certificate: " + certPath);
  }

  const auto notBefore = asn1TimeToTimePoint(X509_get0_notBefore(leaf.get()));
  const auto notAfter = asn1TimeToTimePoint(X509_get0_notAfter(leaf.get()));
  if (!notBefore || !notAfter) {
    return std::unexpected("cannot parse certificate validity dates: " + certPath);
  }

  CertInfo result{
      .subject = commonName(X509_get_subject_name(leaf.get())),
      .issuer = commonName(X509_get_issuer_name(leaf.get())),
      .dnsNames = dnsNames(leaf.get()),
      .notBefore = *notBefore,
      .notAfter = *notAfter,
      .chain = {},
  };

  // Walk every certificate in the PEM, leaf first, recording each link's subject/issuer CNs. The
  // leaf is already parsed; subsequent PEM_read_bio_X509 calls return the next cert until the BIO
  // is exhausted (it then returns null, which ends the loop).
  for (X509* cur = leaf.get(); cur != nullptr;) {
    result.chain.push_back({.subject = commonName(X509_get_subject_name(cur)),
                            .issuer = commonName(X509_get_issuer_name(cur)),
                            .organization = organizationName(X509_get_subject_name(cur)),
                            .issuerOrganization = organizationName(X509_get_issuer_name(cur))});
    if (cur != leaf.get()) {
      X509_free(cur);
    }
    cur = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
  }

  // PEM_read_bio_X509 pushes PEM_R_NO_START_LINE onto the thread-local error queue when it hits
  // EOF (the normal loop-exit path above), so the queue is always dirty here. Clear it so a later
  // OpenSSL caller on this thread (e.g. cert_updater) does not misread our leftover as its own.
  ERR_clear_error();
  return result;
}

}  // namespace mm
