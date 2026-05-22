#pragma once

#include <cstdint>
#include <string>

#include "comm/fieldbus_driver.h"

/// @brief Represents a single EtherCAT slave.
///
/// Holds the slave's bus position, immutable identity read from EEPROM,
/// and a reference to the fieldbus driver for SDO and state operations.
class Device {
 public:
  /// @brief Constructs a device, reading identity from the driver at @p slavePosition.
  /// @param slavePosition  1-based position on the EtherCAT bus (0 is reserved for the master).
  /// @param driver         Fieldbus driver; lifetime must exceed that of this object.
  Device(uint16_t slavePosition, mm::comm::FieldbusDriver& driver);

  /// @brief Returns the 1-based position of this slave on the EtherCAT bus.
  uint16_t slavePosition() const;

  /// @brief Human-readable slave name from SII EEPROM.
  const std::string& name() const;

  /// @brief Vendor ID from EEPROM.
  uint32_t vendorId() const;

  /// @brief Product code from EEPROM.
  uint32_t productCode() const;

  /// @brief Revision number from EEPROM.
  uint32_t revisionNumber() const;

  /// @brief Serial number from EEPROM.
  uint32_t serialNumber() const;

 private:
  uint16_t slavePosition_;
  mm::comm::FieldbusDriver& driver_;
  std::string name_;
  uint32_t vendorId_;
  uint32_t productCode_;
  uint32_t revisionNumber_;
  uint32_t serialNumber_;
};
