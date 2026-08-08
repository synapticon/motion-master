#include "api/router.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mm::api {

namespace {

/// The `:name` tokens of a route pattern, in the order uWS will index them.
///
/// uWS addresses path parameters positionally; naming them is this layer's doing, so that a handler
/// asks for `parameter("slavePosition")` rather than `parameter(0)` and stays correct when a route
/// gains a segment.
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

/// Writes a finished response. Must run on the loop thread.
///
/// Backpressure-aware for every response, not only the large ones: `tryEnd` sends what the socket
/// will take now and `onWritable` resumes from the acknowledged offset. A recorder dump is tens of
/// megabytes and an ESI parse a few, so this is not hypothetical — and handling it here once means
/// no endpoint has to know its own body might be too big to write in one go.
void writeResponse(uWS::HttpResponse<true>* res, std::string_view corsOrigin,
                   const Response& response) {
  auto* r = res->writeStatus(response.status)
                ->writeHeader("Access-Control-Allow-Origin", corsOrigin)
                ->writeHeader("Content-Type", response.contentType);
  for (const auto& [name, value] : response.headers) {
    r = r->writeHeader(name, value);
  }
  // Kept alive for the resumed writes: onWritable can fire long after this returns.
  auto body = std::make_shared<std::string>(response.body);
  auto [ok, done] = res->tryEnd(*body, body->size());
  if (done || ok) {
    return;
  }
  res->onWritable([res, body](uintptr_t offset) {
    auto [chunkOk, chunkDone] = res->tryEnd(std::string_view(*body).substr(offset), body->size());
    return chunkOk || chunkDone;
  });
}

}  // namespace

std::optional<std::string_view> Request::query(std::string_view key) const {
  std::string_view rest = queryString_;
  while (!rest.empty()) {
    const std::size_t amp = rest.find('&');
    std::string_view pair = rest.substr(0, amp);
    rest = amp == std::string_view::npos ? std::string_view{} : rest.substr(amp + 1);

    const std::size_t eq = pair.find('=');
    const std::string_view name = pair.substr(0, eq);
    if (name == key) {
      return eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);
    }
  }
  return std::nullopt;
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

void Router::add(std::string_view method, std::string pattern, Handler handler, bool hasBody) {
  auto names = std::make_shared<std::vector<std::string>>(parameterNames(pattern));
  auto shared = std::make_shared<Handler>(std::move(handler));
  auto* loop = loop_;
  auto* pool = &pool_;
  const std::string_view corsOrigin = corsOrigin_;

  // Everything a handler could read is copied here, on the loop thread, because `req` dies when
  // this returns. Dispatch then happens either immediately or once the body is complete.
  auto onRequest = [names, shared, loop, pool, corsOrigin, hasBody](uWS::HttpResponse<true>* res,
                                                                    uWS::HttpRequest* req) {
    auto parameters = std::vector<std::pair<std::string, std::string>>{};
    parameters.reserve(names->size());
    for (std::size_t i = 0; i < names->size(); ++i) {
      parameters.emplace_back((*names)[i], std::string(req->getParameter(i)));
    }
    auto url = std::string(req->getUrl());
    auto queryString = std::string(req->getQuery());

    // Set before any dispatch: a client can disconnect while the handler is still working, and this
    // flag is the only thing that keeps the deferred write off a destroyed response. Both this and
    // the check in the deferred callback run on the loop thread, so they cannot interleave.
    auto aborted = std::make_shared<std::atomic<bool>>(false);
    res->onAborted([aborted]() { aborted->store(true); });

    auto dispatch = [shared, loop, pool, corsOrigin, res, aborted](
                        std::string url,
                        std::vector<std::pair<std::string, std::string>> parameters,
                        std::string queryString, std::string body) {
      pool->detach_task([shared, loop, corsOrigin, res, aborted, url = std::move(url),
                         parameters = std::move(parameters), queryString = std::move(queryString),
                         body = std::move(body)]() mutable {
        const Request request(std::move(url), std::move(parameters), std::move(queryString),
                              std::move(body));
        Response response = (*shared)(request);
        loop->defer([corsOrigin, res, aborted, response = std::move(response)]() {
          if (aborted->load()) {
            return;  // The connection is gone and res with it.
          }
          writeResponse(res, corsOrigin, response);
        });
      });
    };

    if (!hasBody) {
      dispatch(std::move(url), std::move(parameters), std::move(queryString), std::string{});
      return;
    }
    // A body arrives in chunks; the handler is dispatched once the last one has. Accumulating here
    // rather than in each handler is also what removes the copy of this boilerplate that every
    // body-carrying endpoint used to hold.
    auto body = std::make_shared<std::string>();
    res->onData([dispatch, aborted, body, url = std::move(url), parameters = std::move(parameters),
                 queryString = std::move(queryString)](std::string_view chunk, bool last) mutable {
      body->append(chunk);
      if (!last || aborted->load()) {
        return;
      }
      dispatch(std::move(url), std::move(parameters), std::move(queryString), std::move(*body));
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
