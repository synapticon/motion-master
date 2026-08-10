#include "api/router.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mm::api {

namespace {

/// Writes a finished response. Must run on the loop thread.
///
/// Backpressure-aware for every response, not only the large ones: `tryEnd` sends what the socket
/// will take now and `onWritable` resumes from the acknowledged offset. A recorder dump is tens of
/// megabytes and an ESI parse a few, so this is not hypothetical — and handling it here once means
/// no endpoint has to know its own body might be too big to write in one go.
///
/// **Corked, and that is not an optimisation to skip.** uWS corks the socket around a handler it
/// invokes itself (`HttpContext`), so writes made synchronously in a handler coalesce into one
/// send. A `Loop::defer` callback runs from the loop's wakeup queue instead, outside that cork —
/// so an uncorked write here would issue a separate `us_socket_write`, and so a separate TLS
/// record and syscall, for the status line, for *each* header, and for the body. `cork` also
/// carries the shutdown that a `Connection: close` response owes its client, which otherwise lives
/// only in the path uWS uses for its own synchronous handlers. Corking is what puts a deferred
/// write back on equal footing with an inline one; it is safe unconditionally, since uWS runs the
/// callback uncorked when the loop's single cork slot is taken.
void writeResponse(uWS::HttpResponse<true>* res, std::string_view corsOrigin, Response response) {
  // Kept alive for the resumed writes: onWritable can fire long after this returns. Moved rather
  // than copied — a recorder dump is tens of megabytes, and taking the response by value here is
  // what makes the move possible all the way from the handler that produced it.
  auto body = std::make_shared<std::string>(std::move(response.body));
  bool pending = false;
  res->cork([res, corsOrigin, &response, &body, &pending]() {
    auto* r = res->writeStatus(response.status)
                  ->writeHeader("Access-Control-Allow-Origin", corsOrigin)
                  ->writeHeader("Content-Type", response.contentType);
    for (const auto& [name, value] : response.headers) {
      r = r->writeHeader(name, value);
    }
    auto [ok, done] = res->tryEnd(*body, body->size());
    pending = !done && !ok;
  });
  if (!pending) {
    return;
  }
  res->onWritable([res, body](uintptr_t offset) {
    // Corked for the same reason, and for one more: uWS skips its own connection-close handling
    // while an onWritable is registered, so the cork is what closes a `Connection: close` socket
    // on the write that finally completes the body.
    bool more = false;
    res->cork([res, &body, offset, &more]() {
      auto [chunkOk, chunkDone] = res->tryEnd(std::string_view(*body).substr(offset), body->size());
      more = chunkOk || chunkDone;
    });
    return more;
  });
}

// Reports the first request that has to wait for a worker, and the return to normal after it.
//
// Running every route on the pool is only safe because the pool cannot be saturated by the client
// population this serves — a handful of local clients, each capped by the browser's ~6 connections
// per origin. That is an argument from arithmetic, so it deserves to be checked against reality
// rather than assumed: if a request ever does queue, the API has started degrading in exactly the
// way the Router exists to prevent, and silence would hide it.
//
// Edge-triggered, so a saturated pool logs twice rather than once per request.
void reportSaturation(const BS::light_thread_pool& pool) {
  static std::atomic<bool> saturated{false};
  const bool queued = pool.get_tasks_queued() > 0;
  bool was = saturated.load(std::memory_order_relaxed);
  if (queued == was || !saturated.compare_exchange_strong(was, queued)) {
    return;
  }
  if (queued) {
    spdlog::warn(
        "HTTP worker pool saturated: all {} workers are busy and requests are now queuing. "
        "Responses will be delayed until one frees.",
        pool.get_thread_count());
  } else {
    spdlog::info("HTTP worker pool no longer saturated");
  }
}

}  // namespace

std::optional<std::string> Request::query(std::string_view key) const {
  // uWS::getDecodedQueryValue expects the query *including* its leading '?'
  // (HttpRequest::getQuery() strips it, which is the form stored here) and decodes in place,
  // mutating what it is given. Hence a fresh copy per call: it keeps the mutation confined to that
  // copy, so two lookups cannot corrupt each other's values, and it is why this returns an owned
  // string.
  std::string buffer;
  buffer.reserve(queryString_.size() + 1);
  buffer.push_back('?');
  buffer.append(queryString_);
  const std::string_view value = uWS::getDecodedQueryValue(key, buffer);
  if (value.empty()) {
    return std::nullopt;  // Absent, or present with an empty value — uWS does not distinguish.
  }
  return std::string(value);
}

std::string percentDecode(std::string_view text) {
  std::string decoded;
  decoded.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    uint32_t byte = 0;
    const char* first = text.data() + i + 1;
    const char* last = text.data() + i + 3;
    // Both hex digits must be consumed: from_chars would happily read `%4Z` as 4 and leave the `Z`,
    // which would silently drop a character the user typed. Comparing the whole result against
    // {last, errc{}} is what demands it reached the end with no error.
    if (text[i] == '%' && i + 2 < text.size() &&
        std::from_chars(first, last, byte, 16) == std::from_chars_result{last, std::errc()}) {
      decoded.push_back(static_cast<char>(byte));
      i += 2;
    } else {
      decoded.push_back(text[i]);
    }
  }
  return decoded;
}

Response json(const nlohmann::json& body) {
  return Response{.status = "200 OK",
                  .contentType = "application/json",
                  .body = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                  .headers = {}};
}

Response bytes(std::string contentType, std::string body) {
  return Response{.status = "200 OK",
                  .contentType = std::move(contentType),
                  .body = std::move(body),
                  .headers = {}};
}

Response error(std::string status, std::string_view message) {
  return Response{.status = std::move(status),
                  .contentType = "application/json",
                  .body = nlohmann::json{{"error", std::string(message)}}.dump(),
                  .headers = {}};
}

Response badRequest(std::string_view message) { return error("400 Bad Request", message); }

Response notFound(std::string_view message) { return error("404 Not Found", message); }

Response statusOnly(std::string status) {
  return Response{
      .status = std::move(status), .contentType = "application/json", .body = {}, .headers = {}};
}

Response withWireTime(Response response, std::chrono::microseconds wireUs) {
  response.headers.emplace_back("Access-Control-Expose-Headers", "X-Wire-Us");
  response.headers.emplace_back("X-Wire-Us", std::to_string(wireUs.count()));
  return response;
}

std::vector<std::string> parameterNames(std::string_view pattern) {
  std::vector<std::string> names;
  for (std::size_t at = pattern.find(':'); at != std::string_view::npos;
       at = pattern.find(':', at + 1)) {
    const std::size_t end = pattern.find('/', at);
    names.emplace_back(pattern.substr(
        at + 1, end == std::string_view::npos ? std::string_view::npos : end - at - 1));
  }
  return names;
}

void Router::add(std::string_view method, const std::string& pattern, Handler handler,
                 bool hasBody) {
  auto names = std::make_shared<std::vector<std::string>>(parameterNames(pattern));
  auto shared = std::make_shared<Handler>(std::move(handler));
  auto* loop = loop_;
  auto* pool = &pool_;
  const auto* stopping = &stopping_;
  const std::string_view corsOrigin = corsOrigin_;

  // Everything a handler could read is copied here, on the loop thread, because `req` dies when
  // this returns. Dispatch then happens either immediately or once the body is complete.
  auto onRequest = [names, shared, loop, pool, stopping, corsOrigin, hasBody](
                       uWS::HttpResponse<true>* res, uWS::HttpRequest* req) {
    // Refused rather than dispatched once shutdown has begun, because a worker started now could
    // outlive the loop it would defer its response onto — see Router::stopping_. Answering here is
    // legal (and better than dropping the request) precisely because this runs on the loop thread,
    // which is the one thread allowed to touch a response.
    if (stopping->load(std::memory_order_relaxed)) {
      writeResponse(res, corsOrigin, error("503 Service Unavailable", "server is shutting down"));
      return;
    }

    auto parameters = std::vector<std::pair<std::string, std::string>>{};
    parameters.reserve(names->size());
    for (std::size_t i = 0; i < names->size(); ++i) {
      parameters.emplace_back((*names)[i], std::string(req->getParameter(i)));
    }
    auto url = std::string(req->getUrl());
    auto queryString = std::string(req->getQuery());
    // Headers are copied wholesale rather than by name, because the snapshot is taken before any
    // handler runs and cannot know which ones that handler will ask for.
    std::vector<std::pair<std::string, std::string>> headers;
    for (auto [key, value] : *req) {
      headers.emplace_back(std::string(key), std::string(value));
    }

    // Set before any dispatch: a client can disconnect while the handler is still working, and this
    // flag is the only thing that keeps the deferred write off a destroyed response. Both this and
    // the check in the deferred callback run on the loop thread, so they cannot interleave.
    auto aborted = std::make_shared<std::atomic<bool>>(false);
    res->onAborted([aborted]() { aborted->store(true); });

    auto dispatch = [shared, loop, pool, corsOrigin, res, aborted](
                        std::string url,
                        std::vector<std::pair<std::string, std::string>> parameters,
                        std::string queryString,
                        std::vector<std::pair<std::string, std::string>> headers,
                        std::string body) {
      reportSaturation(*pool);
      pool->detach_task([shared, loop, corsOrigin, res, aborted, url = std::move(url),
                         parameters = std::move(parameters), queryString = std::move(queryString),
                         headers = std::move(headers), body = std::move(body)]() mutable {
        const Request request(std::move(url), std::move(parameters), std::move(queryString),
                              std::move(headers), std::move(body));
        Response response = (*shared)(request);
        loop->defer([corsOrigin, res, aborted, response = std::move(response)]() mutable {
          if (aborted->load()) {
            return;  // The connection is gone and res with it.
          }
          writeResponse(res, corsOrigin, std::move(response));
        });
      });
    };

    if (!hasBody) {
      dispatch(std::move(url), std::move(parameters), std::move(queryString), std::move(headers),
               std::string{});
      return;
    }
    // A body arrives in chunks; the handler is dispatched once the last one has. Accumulating here
    // rather than in each handler is also what removes the copy of this boilerplate that every
    // body-carrying endpoint used to hold.
    auto body = std::make_shared<std::string>();
    res->onData([dispatch, aborted, body, url = std::move(url), parameters = std::move(parameters),
                 queryString = std::move(queryString),
                 headers = std::move(headers)](std::string_view chunk, bool last) mutable {
      body->append(chunk);
      if (!last || aborted->load()) {
        return;
      }
      dispatch(std::move(url), std::move(parameters), std::move(queryString), std::move(headers),
               std::move(*body));
    });
  };

  if (method == "GET") {
    app_.get(pattern, onRequest);
  } else if (method == "POST") {
    app_.post(pattern, onRequest);
  } else if (method == "PUT") {
    app_.put(pattern, onRequest);
  } else {
    app_.del(pattern, onRequest);
  }
}

}  // namespace mm::api
