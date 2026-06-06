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

// Extracts the common name (CN) from an X509_NAME, or "" if absent.
std::string commonName(X509_NAME* name) {
  if (name == nullptr) {
    return {};
  }
  char buf[256];
  const int len = X509_NAME_get_text_by_NID(name, NID_commonName, buf, sizeof(buf));
  if (len < 0) {
    return {};
  }
  return std::string(buf, static_cast<std::size_t>(len));
}

}  // namespace

std::expected<CertInfo, std::string> readCertInfo(const std::string& certPath) {
  const std::unique_ptr<BIO, decltype(&BIO_free)> bio{BIO_new_file(certPath.c_str(), "r"),
                                                      BIO_free};
  if (!bio) {
    return std::unexpected("cannot open certificate file: " + certPath);
  }

  const std::unique_ptr<X509, decltype(&X509_free)> cert{
      PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free};
  if (!cert) {
    return std::unexpected("cannot parse PEM certificate: " + certPath);
  }

  return CertInfo{
      .subject = commonName(X509_get_subject_name(cert.get())),
      .issuer = commonName(X509_get_issuer_name(cert.get())),
      .notBefore = asn1TimeToTimePoint(X509_get0_notBefore(cert.get())),
      .notAfter = asn1TimeToTimePoint(X509_get0_notAfter(cert.get())),
  };
}

}  // namespace mm
