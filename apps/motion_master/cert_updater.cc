#include "cert_updater.h"

#include <curl/curl.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <system_error>

#include "cert_info.h"

namespace mm {

namespace {

// Common name the fetched certificate must carry to be accepted.
constexpr char kCertCommonName[] = "local.motion-master.synapticon.com";

size_t appendToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  const size_t bytes = size * nmemb;
  out->append(ptr, bytes);
  return bytes;
}

// Downloads @p url over HTTPS, following redirects (release asset URLs 302 to a separate host) and
// failing on any HTTP status >= 400. Returns the response body, or an error string.
std::expected<std::string, std::string> httpGet(const std::string& url) {
  // curl_global_init runs once at the composition root (main.cc) before any thread starts; here we
  // only create per-call easy handles, which is safe from any thread.
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return std::unexpected("failed to initialise HTTP client");
  }
  const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> guard{curl, curl_easy_cleanup};

  std::string body;
  char errbuf[CURL_ERROR_SIZE] = {0};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendToString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "motion-master");
  // httpGet also runs off the main thread (POST /api/cert/refresh). With the synchronous resolver
  // libcurl may raise signals (SIGPIPE on a dead socket, historically SIGALRM around timeouts),
  // which is unsafe off the main thread — disable them.
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  const CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    const std::string detail = errbuf[0] != '\0' ? errbuf : curl_easy_strerror(rc);
    return std::unexpected("download failed for " + url + ": " + detail);
  }
  return body;
}

std::string subjectCommonName(X509* cert) {
  char buf[256];
  const int len =
      X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, buf, sizeof(buf));
  if (len < 0) {
    return {};
  }
  // len is the CN's full length, but the API copies at most sizeof(buf)-1 bytes and
  // null-terminates, so clamp to what actually landed in buf — a CN >= sizeof(buf) would otherwise
  // over-read.
  return std::string(buf, std::min(static_cast<std::size_t>(len), sizeof(buf) - 1));
}

// Validates the downloaded pair before it is allowed anywhere near the live files: the cert parses
// and carries the expected CN, it is not already expired, and the key parses and matches the cert.
std::expected<void, std::string> validatePair(const std::string& certPem,
                                              const std::string& keyPem) {
  const std::unique_ptr<BIO, decltype(&BIO_free)> certBio{
      BIO_new_mem_buf(certPem.data(), static_cast<int>(certPem.size())), BIO_free};
  const std::unique_ptr<X509, decltype(&X509_free)> cert{
      PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr), X509_free};
  if (!cert) {
    return std::unexpected("downloaded certificate is not valid PEM");
  }

  const std::string cn = subjectCommonName(cert.get());
  if (cn != kCertCommonName) {
    return std::unexpected("downloaded certificate CN '" + cn + "' != expected '" +
                           std::string(kCertCommonName) + "'");
  }

  // X509_cmp_current_time returns > 0 when notAfter is in the future, i.e. not yet expired.
  if (X509_cmp_current_time(X509_get0_notAfter(cert.get())) <= 0) {
    return std::unexpected("downloaded certificate is already expired");
  }

  const std::unique_ptr<BIO, decltype(&BIO_free)> keyBio{
      BIO_new_mem_buf(keyPem.data(), static_cast<int>(keyPem.size())), BIO_free};
  const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{
      PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free};
  if (!key) {
    return std::unexpected("downloaded key is not valid PEM");
  }
  if (X509_check_private_key(cert.get(), key.get()) != 1) {
    return std::unexpected("downloaded key does not match certificate");
  }
  return {};
}

// Writes @p data to a sibling temp file, applies @p perms, then atomically renames it over @p path.
std::expected<void, std::string> writeAtomic(const std::string& path, const std::string& data,
                                             std::filesystem::perms perms) {
  std::filesystem::path target{path};
  std::filesystem::path tmp{path + ".new"};
  {
    std::ofstream f{tmp, std::ios::binary | std::ios::trunc};
    if (!f) {
      return std::unexpected("cannot open " + tmp.string() + " for writing");
    }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f) {
      return std::unexpected("write failed for " + tmp.string());
    }
  }

  std::error_code ec;
  std::filesystem::permissions(tmp, perms, ec);  // best-effort; key perms matter most
  std::filesystem::rename(tmp, target, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return std::unexpected("cannot replace " + target.string() + ": " + ec.message());
  }
  return {};
}

}  // namespace

std::expected<void, std::string> fetchAndSwapCert(const std::string& certPath,
                                                  const std::string& keyPath,
                                                  const std::string& certUrl,
                                                  const std::string& keyUrl) {
  auto certPem = httpGet(certUrl);
  if (!certPem) {
    return std::unexpected(certPem.error());
  }
  auto keyPem = httpGet(keyUrl);
  if (!keyPem) {
    return std::unexpected(keyPem.error());
  }

  if (auto r = validatePair(*certPem, *keyPem); !r) {
    return r;
  }

  // Install the key first (0600) then the cert; both pass through a temp-then-rename so a failed
  // write never leaves a half-written live file. The pair was validated above, so the brief window
  // between the two renames still holds a matching cert/key.
  using std::filesystem::perms;
  if (auto r = writeAtomic(keyPath, *keyPem, perms::owner_read | perms::owner_write); !r) {
    return r;
  }
  if (auto r = writeAtomic(
          certPath, *certPem,
          perms::owner_read | perms::owner_write | perms::group_read | perms::others_read);
      !r) {
    return r;
  }
  return {};
}

ResolvedCert resolveCertPaths(const std::string& configCertPath, const std::string& configKeyPath,
                              const std::filesystem::path& defaultCertPath,
                              const std::filesystem::path& defaultKeyPath) {
  // Both already set by the config file — use them as-is and discover nothing.
  if (!configCertPath.empty() && !configKeyPath.empty()) {
    return {.certPath = configCertPath, .keyPath = configKeyPath, .source = {}};
  }

  // 1. cert.pem / key.pem next to the binary (a release install).
  if (std::filesystem::exists(defaultCertPath) && std::filesystem::exists(defaultKeyPath)) {
    return {.certPath = defaultCertPath.string(),
            .keyPath = defaultKeyPath.string(),
            .source = "bundled cert (" + defaultCertPath.string() + ")"};
  }

  // 2. A local acme.sh install, renewed automatically by its own cron.
  if (const char* home = std::getenv("HOME")) {
    const auto acmeDir =
        std::filesystem::path(home) / ".acme.sh/local.motion-master.synapticon.com_ecc";
    const auto acmeCert = acmeDir / "fullchain.cer";
    const auto acmeKey = acmeDir / "local.motion-master.synapticon.com.key";
    if (std::filesystem::exists(acmeCert) && std::filesystem::exists(acmeKey)) {
      return {.certPath = acmeCert.string(),
              .keyPath = acmeKey.string(),
              .source = "Let's Encrypt cert from acme.sh (" + acmeCert.string() + ")"};
    }
  }

  // 3. Nothing found — target the install-dir default so the caller's self-heal can populate it.
  return {.certPath = defaultCertPath.string(), .keyPath = defaultKeyPath.string(), .source = {}};
}

std::expected<void, std::string> healCertIfNeeded(const std::string& certPath,
                                                  const std::string& keyPath, bool autoUpdate,
                                                  const std::string& certUrl,
                                                  const std::string& keyUrl) {
  // Assess the served cert and refresh it when it is missing, expired, or expiring soon. A missing
  // cert means we cannot serve TLS at all; an expired or soon-to-expire cert still binds (and stays
  // served if the fetch fails), but is refreshed proactively so an ephemeral container — or the
  // entrypoint's 1-day self-signed fallback, which reads as expiring soon — self-heals to a fresh
  // cert on start. A cert with ample life left is left alone, so a healthy boot makes no network
  // call.
  const bool certMissing = !std::filesystem::exists(certPath) || !std::filesystem::exists(keyPath);
  bool expired = false;
  bool expiringSoon = false;
  if (certMissing) {
    spdlog::warn("No TLS certificate at {}", certPath);
  } else if (auto info = readCertInfo(certPath)) {
    const auto now = std::chrono::system_clock::now();
    const auto daysRemaining =
        std::chrono::duration_cast<std::chrono::hours>(info->notAfter - now).count() / 24;
    if (now >= info->notAfter) {
      expired = true;
      spdlog::error("TLS certificate EXPIRED ({} days ago)", -daysRemaining);
    } else if (daysRemaining < kCertExpiringSoonDays) {
      expiringSoon = true;
      spdlog::warn("TLS certificate expires in {} days", daysRemaining);
    } else {
      spdlog::info("TLS certificate valid for {} more days", daysRemaining);
    }
  } else {
    spdlog::warn("Could not read TLS certificate expiry: {}", info.error());
  }

  if (!certMissing && !expired && !expiringSoon) {
    return {};
  }

  if (!autoUpdate) {
    if (certMissing) {
      return std::unexpected("no certificate and cert auto-update is disabled — cannot serve TLS");
    }
    spdlog::warn("Cert auto-update disabled — serving the current certificate as-is");
    return {};
  }

  spdlog::info("Fetching fresh TLS certificate from {}", certUrl);
  if (auto r = fetchAndSwapCert(certPath, keyPath, certUrl, keyUrl); r) {
    spdlog::info("Installed fresh TLS certificate at {}", certPath);
    return {};
  } else if (certMissing) {
    return std::unexpected("certificate fetch failed and no local certificate exists: " +
                           r.error());
  } else {
    spdlog::error("Certificate fetch failed: {} — serving the existing certificate", r.error());
    return {};
  }
}

}  // namespace mm
