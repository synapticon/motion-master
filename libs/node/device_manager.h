#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

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
  /// Must be called before @c scan() and @c pdoExchange(). Any previously
  /// held driver is replaced.
  ///
  /// @param driver  Concrete fieldbus driver to own and operate.
  /// @return Void on success, or an error string on failure.
  std::expected<void, std::string> init(std::unique_ptr<mm::comm::FieldbusDriver> driver);

  /// @brief Scans the bus for nodes and populates the device list.
  ///
  /// Must be called after @c init(). Forwards to @c FieldbusDriver::scan().
  ///
  /// @return Number of nodes found on success, or an error string on failure.
  std::expected<int, std::string> scan();

  /// @brief Stops the fieldbus driver and clears the device list.
  ///
  /// Transitions all slaves to INIT state, closes the network interface, and
  /// removes all @c Device objects. After this returns, @c init() and
  /// @c scan() may be called again. Must not be called while @c pdoExchange()
  /// is running concurrently.
  void reset();

  /// @brief Returns the list of discovered devices.
  /// @return Devices in bus order (index 0 = node position 1). Empty before @c scan().
  const std::vector<Device>& devices() const;

  /// @brief Exchanges process data with all nodes.
  ///
  /// Called once per @c GameLoop cycle. No-op when no driver is initialised.
  ///
  /// @warning @c pdoExchange() runs on the RT GameLoop thread while @c init(),
  ///          @c scan(), and @c reset() may be called from the HTTP server thread.
  ///          There is currently no lock guarding @c driver_ or @c devices_ across
  ///          that boundary.  This is safe only because @c pdoExchange() is not yet
  ///          wired into the GameLoop.  Before enabling PDO exchange, stop the loop
  ///          (or drain one cycle) before calling @c init() / @c reset() via the API.
  void pdoExchange();

  /// @brief Transitions a set of devices to @p targetState, blocking until all arrive or
  ///        @p timeout elapses.
  ///
  /// If @p positions is empty, all discovered devices are targeted. Devices that do not
  /// arrive within @p timeout are logged at error level; the call still returns successfully.
  ///
  /// Must be called after both @c init() and @c scan().
  ///
  /// @param positions    1-based slave positions to transition; empty = all devices.
  /// @param targetState  Desired EtherCAT AL state.
  /// @param timeout      Maximum time to wait for all devices.
  /// @return Void on success, or an error string if no driver is initialised or no devices
  ///         have been discovered.
  std::expected<void, std::string> transitionToState(const std::vector<uint16_t>& positions,
                                                     mm::comm::EtherCatState targetState,
                                                     std::chrono::steady_clock::duration timeout);

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
