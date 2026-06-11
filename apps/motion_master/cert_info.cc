#include "cert_info.h"

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <ctime>
#include <memory>
#include <string>

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

// Reads an ASN1_TIME (a certificate's notBefore/notAfter) into a time_point.
std::chrono::system_clock::time_point asn1TimeToTimePoint(const ASN1_TIME* t) {
  std::tm tm{};
  ASN1_TIME_to_tm(t, &tm);
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

  CertInfo result{
      .subject = commonName(X509_get_subject_name(leaf.get())),
      .issuer = commonName(X509_get_issuer_name(leaf.get())),
      .notBefore = asn1TimeToTimePoint(X509_get0_notBefore(leaf.get())),
      .notAfter = asn1TimeToTimePoint(X509_get0_notAfter(leaf.get())),
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

  return result;
}

}  // namespace mm
