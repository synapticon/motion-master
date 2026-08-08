#include "example/example_routes.h"

#include <nlohmann/json.hpp>

#include "api/router.h"
#include "example/example_logic.h"
#include "node/device_manager.h"

namespace mm::example {

void registerRoutes(mm::api::Router& router, const mm::api::RouteContext& ctx) {
  // Capture the individual fields, never the RouteContext itself — it is a temporary that dies when
  // registration returns (see the warning on mm::api::RouteContext). The DeviceManager reference
  // outlives the running server, so this capture is safe for the lifetime of every request.
  //
  // The handler takes a Request and returns a Response, so it runs on a worker thread rather than
  // the event loop and may block for as long as the bus makes it — see mm::api::Router.
  router.get("/api/example/devices", [&deviceManager = ctx.deviceManager](const mm::api::Request&) {
    return mm::api::json(nlohmann::json(summarizeDevices(deviceManager)));
  });
}

}  // namespace mm::example
