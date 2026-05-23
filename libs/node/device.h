#pragma once

#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>

#include "comm/fieldbus_driver.h"

namespace mm::node {

/// @brief Represents a single node on the fieldbus.
///
/// Holds the node's bus position, immutable identity read from EEPROM,
/// and a reference to the fieldbus driver for SDO and state operations.
class Device {
 public:
  /// @brief Constructs a device, reading identity from the driver at @p slavePosition.
  /// @param slavePosition  1-based position on the fieldbus (0 is reserved for the master).
  /// @param driver         Fieldbus driver; lifetime must exceed that of this object.
  Device(uint16_t slavePosition, mm::comm::FieldbusDriver& driver);

  /// @brief Returns the 1-based position of this node on the fieldbus.
  uint16_t slavePosition() const;

  /// @brief Human-readable node name from SII EEPROM.
  const std::string& name() const;

  /// @brief Vendor ID from EEPROM.
  uint32_t vendorId() const;

  /// @brief Product code from EEPROM.
  uint32_t productCode() const;

  /// @brief Revision number from EEPROM.
  uint32_t revisionNumber() const;

  /// @brief Serial number from EEPROM.
  uint32_t serialNumber() const;

  /// @brief Reads bytes from an ESC register on this device.
  ///
  /// Delegates to the fieldbus driver's @c readRegister using this device's slave position.
  ///
  /// @param address  ESC register address (e.g. @c 0x0130 for DL Status).
  /// @param data     Output buffer; its size determines how many bytes are read.
  /// @return Void on success, or an error string on failure.
  std::expected<void, std::string> readRegister(uint16_t address, std::span<uint8_t> data);

  /// @brief Writes bytes to an ESC register on this device.
  ///
  /// Delegates to the fieldbus driver's @c writeRegister using this device's slave position.
  ///
  /// @param address  ESC register address.
  /// @param data     Bytes to write.
  /// @return Void on success, or an error string on failure.
  std::expected<void, std::string> writeRegister(uint16_t address, std::span<const uint8_t> data);

 private:
  uint16_t slavePosition_;
  mm::comm::FieldbusDriver& driver_;
  std::string name_;
  uint32_t vendorId_;
  uint32_t productCode_;
  uint32_t revisionNumber_;
  uint32_t serialNumber_;
};

/// @brief Serialises a Device to JSON.
///
/// Produces an object with keys `slavePosition`, `name`, `vendorId`,
/// `productCode`, `revisionNumber`, and `serialNumber`.  Participates in
/// nlohmann ADL so that `nlohmann::json(device)` and `std::vector<Device>`
/// conversions work automatically.
///
/// @param j  Output JSON value.
/// @param d  Device to serialise.
void to_json(nlohmann::json& j, const Device& d);

}  // namespace mm::node
