#pragma once

#include <expected>
#include <string>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "device.h"

/// @brief Owns the EtherCAT device collection and drives PDO exchange.
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

  /// @brief Discovers slaves and populates the device list.
  ///
  /// Must be called after @c init(). Forwards to @c FieldbusDriver::configure().
  ///
  /// @return Number of slaves found on success, or an error string on failure.
  std::expected<int, std::string> configure();

  /// @brief Returns the list of configured devices.
  /// @return Devices in bus order (index 0 = slave position 1). Empty before @c configure().
  const std::vector<Device>& devices() const;

  /// @brief Exchanges process data with all devices.
  ///
  /// Called once per @c GameLoop cycle. Forwards to @c FieldbusDriver::exchangeProcessData().
  void pdoExchange();

 private:
  mm::comm::FieldbusDriver& driver_;
  std::vector<Device> devices_;
};
