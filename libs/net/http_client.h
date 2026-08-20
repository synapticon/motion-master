#pragma once

#include <chrono>
#include <expected>
#include <string>

/// @file
/// @brief A blocking HTTP client for the calls Motion Master makes *out*.
///
/// Motion Master is a server, and this is the small counterpart it needs as a client: it downloads
/// its own TLS certificate from a release, and it drives the auto-tuning child process over HTTP on
/// loopback. Both calls block the thread they run on, which is what every caller wants — each one
/// already runs off the event loop, on a router worker thread or on a startup path.
///
/// libcurl lives behind these two functions. It is linked privately, so no curl type appears in a
/// header and no other target inherits the dependency.

namespace mm::net {

/// @brief What the server answered.
///
/// A transport failure is the error side of @c std::expected, so a @c Response means the request
/// reached a server and it replied. The reply may still be a refusal: @c status carries the HTTP
/// status and the caller decides. Nothing here treats a 4xx as an error, because a caller that
/// proxies a response needs both halves of it.
struct Response {
  int status = 0;    ///< HTTP status code, as the server sent it.
  std::string body;  ///< Response body, however long, exactly as received.
};

/// @brief Performs a blocking GET.
/// @param url     Absolute URL. Redirects are followed, up to ten of them, which release asset
///                URLs need: they answer 302 towards a separate download host.
/// @param timeout Whole-call limit, including connect and transfer.
/// @return The reply, or an error string naming the URL and the transport failure.
std::expected<Response, std::string> httpGet(const std::string& url,
                                             std::chrono::seconds timeout = std::chrono::seconds{
                                                 30});

/// @brief Performs a blocking POST.
/// @param url         Absolute URL. Redirects are followed as for @c httpGet.
/// @param body        Request body, sent with an explicit Content-Length.
/// @param contentType Value of the Content-Type header.
/// @param timeout     Whole-call limit, including connect and transfer.
/// @return The reply, or an error string naming the URL and the transport failure.
std::expected<Response, std::string> httpPost(const std::string& url, const std::string& body,
                                              const std::string& contentType,
                                              std::chrono::seconds timeout = std::chrono::seconds{
                                                  30});

/// @brief Holds libcurl's process-wide state for the lifetime of one instance.
///
/// libcurl requires one global initialisation before any other thread runs, because it also
/// initialises libraries (OpenSSL) that are unsafe to set up concurrently. So the composition root
/// constructs exactly one of these, before it starts a thread, and every later call shares it.
/// Construct it there and nowhere else: a second instance would repeat the initialisation, and a
/// lazy one inside a request would race the very threads it has to precede.
class HttpGlobal {
 public:
  HttpGlobal();
  ~HttpGlobal();
  HttpGlobal(const HttpGlobal&) = delete;
  HttpGlobal& operator=(const HttpGlobal&) = delete;
  HttpGlobal(HttpGlobal&&) = delete;
  HttpGlobal& operator=(HttpGlobal&&) = delete;
};

}  // namespace mm::net
