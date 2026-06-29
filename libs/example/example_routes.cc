#include "example/example_routes.h"

#include <nlohmann/json.hpp>

#include "api/web_api.h"
#include "example/example_logic.h"
#include "node/device_manager.h"

namespace mm::example {

void registerRoutes(uWS::SSLApp& app, const mm::api::RouteContext& ctx) {
  // Capture the individual fields, never the RouteContext itself — it is a temporary that dies when
  // registration returns (see the warning on mm::api::RouteContext). The DeviceManager reference
  // and the storage behind corsOrigin both outlive the running server, so these captures are safe
  // for the lifetime of every request handler.
  app.get("/api/example/devices", [&deviceManager = ctx.deviceManager, corsOrigin = ctx.corsOrigin](
                                      auto* res, auto* /*req*/) {
    mm::api::sendJson(res, corsOrigin, nlohmann::json(summarizeDevices(deviceManager)));
  });
}

}  // namespace mm::example
