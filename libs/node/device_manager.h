#pragma once

#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "comm/fieldbus_driver.h"
#include "node/device.h"

namespace mm::node {

/// @brief Owns the fieldbus driver and node collection, and drives PDO exchange.
///
/// The driver is not required at construction — call @c init() to supply one.
/// This allows the app to start without a driver and be initialised later via
/// the HTTP API. Injected into @c GameLoop (for @c pdoExchange) and
/// @c HttpServer (for SDO/state operations).
class DeviceManager {
 public:
  DeviceManager() = default;

  /// @brief Takes ownership of @p driver and initialises it.
  ///
  /// Must be called before @c configure() and @c pdoExchange(). Any previously
  /// held driver is replaced.
  ///
  /// @param driver  Concrete fieldbus driver to own and operate.
  /// @return Void on success, or an error string on failure.
  std::expected<void, std::string> init(std::unique_ptr<mm::comm::FieldbusDriver> driver);

  /// @brief Discovers nodes and populates the device list.
  ///
  /// Must be called after @c init(). Forwards to @c FieldbusDriver::configure().
  ///
  /// @return Number of nodes found on success, or an error string on failure.
  std::expected<int, std::string> configure();

  /// @brief Stops the fieldbus driver and clears the device list.
  ///
  /// Transitions all slaves to INIT state, closes the network interface, and
  /// removes all @c Device objects. After this returns, @c init() and
  /// @c configure() may be called again. Must not be called while @c pdoExchange()
  /// is running concurrently.
  void reset();

  /// @brief Returns the list of configured devices.
  /// @return Devices in bus order (index 0 = node position 1). Empty before @c configure().
  const std::vector<Device>& devices() const;

  /// @brief Exchanges process data with all nodes.
  ///
  /// Called once per @c GameLoop cycle. No-op when no driver is initialised.
  ///
  /// @warning @c pdoExchange() runs on the RT GameLoop thread while @c init(),
  ///          @c configure(), and @c reset() may be called from the HTTP server thread.
  ///          There is currently no lock guarding @c driver_ or @c devices_ across
  ///          that boundary.  This is safe only because @c pdoExchange() is not yet
  ///          wired into the GameLoop.  Before enabling PDO exchange, stop the loop
  ///          (or drain one cycle) before calling @c init() / @c reset() via the API.
  void pdoExchange();

 private:
  std::unique_ptr<mm::comm::FieldbusDriver> driver_;
  std::vector<Device> devices_;
};

/// @brief Serialises all devices in a DeviceManager to a JSON array.
///
/// Produces a JSON array where each element is the serialised form of a
/// `Device` (see `to_json(nlohmann::json&, const Device&)`), in bus order.
/// Participates in nlohmann ADL so that `nlohmann::json(deviceManager)` works.
///
/// @param j   Output JSON value.
/// @param dm  DeviceManager whose device list to serialise.
void to_json(nlohmann::json& j, const DeviceManager& dm);

}  // namespace mm::node
