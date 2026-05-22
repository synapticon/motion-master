#pragma once

#include <expected>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "comm/fieldbus_driver.h"
#include "node/device.h"

namespace mm::node {

/// @brief Owns the fieldbus node collection and drives PDO exchange.
///
/// Constructed by @c App with a concrete @c FieldbusDriver. Injected into
/// @c GameLoop (for @c pdoExchange) and @c HttpServer (for SDO/state operations).
class DeviceManager {
 public:
  /// @brief Constructs the manager bound to the given fieldbus driver.
  /// @param driver  Lifetime must exceed that of this object.
  explicit DeviceManager(mm::comm::FieldbusDriver& driver);

  /// @brief Initialises the fieldbus driver.
  ///
  /// Must be called before @c pdoExchange(). Forwards to @c FieldbusDriver::init().
  ///
  /// @return Void on success, or an error string on failure.
  std::expected<void, std::string> init();

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
  /// Called once per @c GameLoop cycle. Forwards to @c FieldbusDriver::exchangeProcessData().
  void pdoExchange();

 private:
  mm::comm::FieldbusDriver& driver_;
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
