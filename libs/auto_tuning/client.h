#pragma once

#include <chrono>
#include <expected>
#include <string>
#include <utility>

#include "net/http_client.h"

/// @file
/// @brief Calls made to the running auto-tuning process.
///
/// The auto-tuning program takes one request shape for every function it offers: a JSON object with
/// a @c run naming the function and a @c data holding its inputs. So this class has one call, and
/// the function names are data rather than methods.
///
/// **A refused input arrives as a success.** The program answers a rejected input with HTTP 200 and
/// an @c error property in the body, and reserves 4xx and 5xx for a malformed request, an unknown
/// function, and a routine that threw. A caller therefore tests for @c error in the body, not only
/// the status. Nothing is unwrapped here, because the endpoint that forwards this has to pass both
/// halves through untouched.

namespace mm::auto_tuning {

/// @brief Talks to one running auto-tuning process.
///
/// Stateless apart from the address, so all methods are safe to call from any thread. That is what
/// the HTTP routes need: several requests may be in flight, and the program serves them
/// concurrently.
class Client {
 public:
  /// @brief Binds to a running process.
  /// @param baseUrl Root URL of the program's API, as @c Process::baseUrl reports it.
  explicit Client(std::string baseUrl) : baseUrl_(std::move(baseUrl)) {}

  /// @brief Runs one function.
  ///
  /// @param requestBody A JSON document with @c run and @c data. Passed through as text: this class
  ///                    neither builds nor validates it, because the program is the authority on
  ///                    what its functions accept and answers a bad request itself.
  /// @param timeout     Whole-call limit. The default is far above what any function takes —
  ///                    measured against version 3.1.1, the slowest returns in 0.2 s — so it only
  ///                    fires when the program stops answering.
  /// @return The reply, status and body, whatever the status. An error only when the request did
  ///         not reach the program at all, which for a process on loopback means it died.
  std::expected<mm::net::Response, std::string> run(
      const std::string& requestBody,
      std::chrono::seconds timeout = std::chrono::seconds{30}) const;

  /// @brief Fetches the program's own OpenAPI document.
  ///
  /// This is the authority on the request and result shapes. Motion Master's own @c swagger.yml
  /// carries a copy of them, so that its generated clients are typed, and serving this alongside is
  /// what makes a drift between the two visible.
  std::expected<mm::net::Response, std::string> spec() const;

  /// @brief The address this client was built with.
  const std::string& baseUrl() const { return baseUrl_; }

 private:
  std::string baseUrl_;
};

}  // namespace mm::auto_tuning
