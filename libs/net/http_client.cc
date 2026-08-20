#include "net/http_client.h"

#include <curl/curl.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace mm::net {

namespace {

size_t appendToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  const size_t bytes = size * nmemb;
  out->append(ptr, bytes);
  return bytes;
}

/// @brief Runs one prepared handle and turns the outcome into a @c Response or an error string.
std::expected<Response, std::string> perform(CURL* curl, const std::string& url,
                                             std::chrono::seconds timeout, std::string* body,
                                             char* errbuf) {
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendToString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
  // Every curl option and result below is a C `long`, whose width differs between LP64 and LLP64.
  // A fixed-width type here would read or write the wrong number of bytes on Windows, so the C type
  // is the correct one and cpplint's preference cannot be honoured.
  // NOLINTNEXTLINE(runtime/int)
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout.count()));
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "motion-master");
  // These calls run off the main thread: a certificate refresh comes from POST /api/cert/refresh,
  // an auto-tuning call from a router worker. With the synchronous resolver libcurl may raise
  // signals (SIGPIPE on a dead socket, historically SIGALRM around timeouts), which is unsafe off
  // the main thread. Disable them.
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  const CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    const std::string detail = errbuf[0] != '\0' ? errbuf : curl_easy_strerror(rc);
    return std::unexpected("request failed for " + url + ": " + detail);
  }

  // NOLINTNEXTLINE(runtime/int)
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  return Response{static_cast<int>(status), std::move(*body)};
}

}  // namespace

std::expected<Response, std::string> httpGet(const std::string& url, std::chrono::seconds timeout) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return std::unexpected("failed to initialise HTTP client");
  }
  const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> guard{curl, curl_easy_cleanup};

  std::string body;
  char errbuf[CURL_ERROR_SIZE] = {0};
  return perform(curl, url, timeout, &body, errbuf);
}

std::expected<Response, std::string> httpPost(const std::string& url, const std::string& body,
                                              const std::string& contentType,
                                              std::chrono::seconds timeout) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return std::unexpected("failed to initialise HTTP client");
  }
  const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> guard{curl, curl_easy_cleanup};

  // curl_slist_append copies the string, so the header list needs no storage of ours to outlive it.
  curl_slist* headers = curl_slist_append(nullptr, ("Content-Type: " + contentType).c_str());
  const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headerGuard{
      headers, curl_slist_free_all};

  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  // COPYPOSTFIELDS would duplicate a body that can be hundreds of kilobytes. POSTFIELDS keeps a
  // pointer instead, and the caller's body outlives this call, so the pointer stays valid.
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  std::string response;
  char errbuf[CURL_ERROR_SIZE] = {0};
  return perform(curl, url, timeout, &response, errbuf);
}

HttpGlobal::HttpGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }

HttpGlobal::~HttpGlobal() { curl_global_cleanup(); }

}  // namespace mm::net
