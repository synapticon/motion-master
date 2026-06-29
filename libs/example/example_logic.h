#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace mm::node {
class DeviceManager;
}  // namespace mm::node

/// @brief Example application namespace — the starting point for a C++ HTTP-endpoint plug-in.
///
/// Everything here is meant to be **copied and renamed**: copy `libs/example`, rename the
/// directory, the `mm::example` namespace, and the route prefix, then replace this domain code and
/// these routes with your own. The split mirrors the rest of the codebase: real work lives in
/// plain, HTTP-agnostic functions here (testable without a server), and `example_routes.cc` is the
/// thin layer that parses requests, calls these functions, and formats responses.
namespace mm::example {

/// @brief A trimmed, presentation-shaped view of one device — the kind of derived data an
/// application-specific endpoint typically returns (vs. the full `Device` JSON the built-in
/// `/api/devices` route serves).
struct DeviceSummary {
  uint16_t position{};      ///< 1-based slave position on the bus.
  std::string name;         ///< Device name as reported over the fieldbus.
  std::string vendorId;     ///< Vendor ID, formatted as `0x........`.
  std::string productCode;  ///< Product code, formatted as `0x........`.
  uint32_t serialNumber{};  ///< Raw serial number.
};

/// @brief Serialises a @c DeviceSummary to JSON. Lives next to the type (ADL-found by nlohmann) so
/// a `nlohmann::json(summary)` or `nlohmann::json(vector<DeviceSummary>)` just works.
void to_json(nlohmann::json& j, const DeviceSummary& summary);

/// @brief Builds a summary row for every device currently known to @p deviceManager.
///
/// Pure read-only domain logic — no HTTP, no I/O — so it is unit-testable against a
/// @c DeviceManager with no live bus (returns an empty vector when nothing has been scanned).
std::vector<DeviceSummary> summarizeDevices(const mm::node::DeviceManager& deviceManager);

}  // namespace mm::example
