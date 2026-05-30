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

/// @brief Current AL state snapshot for a single device.
struct DeviceStateInfo {
  uint16_t slavePosition;  ///< 1-based position on the fieldbus.
  uint16_t alStatus;       ///< Raw AL Status register (bits 3:0 = state, bit 4 = error indicator).
  uint16_t alState;  ///< AL state decoded from alStatus (1=Init, 2=PreOp, 3=Boot, 4=SafeOp, 8=Op).
  bool error;        ///< True when the AL Status error indicator bit is set.
  uint16_t alStatusCode;  ///< AL Status Code (ETG.1000.6 §6.4.1); non-zero when error is true.
};

/// @brief Serialises a DeviceStateInfo to JSON.
void to_json(nlohmann::json& j, const DeviceStateInfo& info);

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
  /// Must be called before @c scan() and @c pdoExchange(). One-shot: fails if a
  /// driver is already held — call @c reset() first. Replacing a live driver
  /// would dangle the @c FieldbusDriver& that every @c Device holds.
  ///
  /// @param driver  Concrete fieldbus driver to own and operate.
  /// @return Void on success, or an error string if a driver is already held or
  ///         driver initialisation fails.
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

  /// @brief Whether a driver is currently held (i.e. @c init() has succeeded and
  ///        @c reset() has not since been called).
  bool initialised() const { return driver_ != nullptr; }

  /// @brief Returns the list of discovered devices.
  /// @return Devices in bus order (index 0 = node position 1). Empty before @c scan().
  const std::vector<Device>& devices() const;

  /// @brief Finds a device by its 1-based bus position.
  ///
  /// @param slavePosition  1-based position of the device on the fieldbus.
  /// @return Pointer to the matching @c Device, or @c nullptr if not found.
  const Device* findDevice(uint16_t slavePosition) const;

  /// @brief Mutable overload of @c findDevice.
  ///
  /// Hands back a writable @c Device so SDK callers can drive it directly —
  /// e.g. @c dm.findDevice(1)->writeValue(0x2030, 1, 123). @c nullptr if not found.
  Device* findDevice(uint16_t slavePosition);

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
  ///        @p timeout elapses, and reports the final state of each.
  ///
  /// If @p positions is empty, all discovered devices are targeted. After the transition
  /// settles, the final AL state of every targeted device is read back and returned — so
  /// callers see exactly where each device ended up rather than a bare success flag. A
  /// device reached the target when its returned snapshot has @c error clear and
  /// @c alState equal to @p targetState; otherwise @c alStatusCode explains why.
  ///
  /// Must be called after both @c init() and @c scan().
  ///
  /// @param positions    1-based slave positions to transition; empty = all devices.
  /// @param targetState  Desired EtherCAT AL state.
  /// @param timeout      Maximum time to wait for all devices.
  /// @return The final state snapshot of each targeted device (in the order targeted), or an
  ///         error string if no driver is initialised, no devices have been discovered, or the
  ///         final state read-back fails.
  std::expected<std::vector<DeviceStateInfo>, std::string> transitionToState(
      const std::vector<uint16_t>& positions, mm::comm::EtherCatState targetState,
      std::chrono::steady_clock::duration timeout);

  /// @brief Reads the current AL state for a set of devices.
  ///
  /// If @p positions is empty, all discovered devices are queried.
  /// Must be called after both @c init() and @c scan().
  ///
  /// @param positions  1-based slave positions to query; empty = all devices.
  /// @return AL state snapshot per device, or an error string if the driver is
  ///         not initialised or the hardware read fails.
  std::expected<std::vector<DeviceStateInfo>, std::string> getDeviceStates(
      const std::vector<uint16_t>& positions);

  /// @brief Enumerates the CoE object dictionary of one device and populates its
  ///        parameter map. See @c Device::initializeParameters for details.
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @param readValues     When @c true, also issue an SDO upload for each entry
  ///                       and store the decoded value on the parameter.
  /// @return Void on success, an error string if the device is unknown, or if
  ///         the OD enumeration itself fails.
  std::expected<void, std::string> initializeDeviceParameters(uint16_t slavePosition,
                                                              bool readValues);

  /// @brief Convenience: finds a device by position and reads one of its parameters.
  ///
  /// Equivalent to @c findDevice(slavePosition)->readParameter(index, subindex) — see
  /// @c Device::readParameter for the online/offline semantics. Saves callers a manual
  /// lookup when they only have a position.
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @param index          CoE object index.
  /// @param subindex       CoE object subindex.
  /// @return The value, or an error string if the device or parameter is unknown, or the
  ///         (online) SDO upload fails.
  std::expected<DeviceParameterValue, std::string> readDeviceParameter(uint16_t slavePosition,
                                                                       uint16_t index,
                                                                       uint8_t subindex);

  /// @brief Convenience: finds a device by position and writes one of its parameters.
  ///
  /// Equivalent to @c findDevice(slavePosition)->writeParameter(index, subindex, value) —
  /// see @c Device::writeParameter for the online/offline semantics (offline edits succeed
  /// and are held as @c SyncState::Pending).
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @param index          CoE object index.
  /// @param subindex       CoE object subindex.
  /// @param value          Value to set; coerced to the parameter's declared type.
  /// @return Void on success, or an error string if the device or parameter is unknown,
  ///         the value cannot be coerced, or an online download fails.
  std::expected<void, std::string> writeDeviceParameter(uint16_t slavePosition, uint16_t index,
                                                        uint8_t subindex,
                                                        DeviceParameterValue value);

 private:
  /// @brief Updates a device's online flag from a freshly-read AL state.
  ///
  /// Online means the SDO mailbox is available — AL state PRE-OP, SAFE-OP, or OP with no
  /// error indicator. INIT and BOOT (and any error state) count as offline. No-op if the
  /// device is unknown.
  void updateOnline(const DeviceStateInfo& info);

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
