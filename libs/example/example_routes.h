#pragma once

#include <uwebsockets/App.h>

#include "api/web_api.h"

namespace mm::example {

/// @brief Registers the example application's HTTP routes (`/api/example/...`) on @p app.
///
/// This is a @c mm::api::RegisterRoutesFn — wire it into the server in the composition root with
/// @c HttpServer::addRoutes(mm::example::registerRoutes) before @c start(). It is the C++ analogue
/// of the `web/apps/example` PWA: a minimal, copy-me starting point for adding your own endpoints.
///
/// Called once, on the HTTP event-loop thread, with @p ctx bound to the live device and monitoring
/// managers. Handlers registered here run on that same loop thread for every request.
void registerRoutes(uWS::SSLApp& app, const mm::api::RouteContext& ctx);

}  // namespace mm::example
