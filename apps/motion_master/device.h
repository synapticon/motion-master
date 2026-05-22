#pragma once

#include <cstdint>

#include "comm/fieldbus_driver.h"

/// @brief Represents a single EtherCAT slave.
///
/// Holds the slave's position index on the bus and a reference to the
/// fieldbus driver for SDO and state operations.
class Device {
 public:
  /// @brief Constructs a device for the given slave position.
  /// @param slavePosition  1-based position on the EtherCAT bus (0 is reserved for the master).
  /// @param driver         Fieldbus driver; lifetime must exceed that of this object.
  Device(uint16_t slavePosition, mm::comm::FieldbusDriver& driver);

  /// @brief Returns the 1-based position of this slave on the EtherCAT bus.
  uint16_t slavePosition() const;

 private:
  uint16_t slavePosition_;
  mm::comm::FieldbusDriver& driver_;
};
