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

  /// @brief Exchanges process data with all devices.
  ///
  /// Called once per @c GameLoop cycle. Forwards to @c FieldbusDriver::exchangeProcessData().
  void pdoExchange();

 private:
  mm::comm::FieldbusDriver& driver_;
  std::vector<Device> devices_;
};
