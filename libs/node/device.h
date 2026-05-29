#pragma once

#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "node/device_parameter.h"

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

  /// @brief Uploads an object dictionary entry from the device (CoE SDO upload).
  ///
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @return The bytes transferred on success, or an error string if the mailbox transfer fails.
  std::expected<std::vector<uint8_t>, std::string> upload(uint16_t index, uint8_t subindex) const;

  /// @brief Reads a file from this device via File over EtherCAT (FoE).
  ///
  /// @param filename  FoE filename as recognised by the slave firmware.
  /// @return File bytes on success, or an error string if the transfer fails.
  std::expected<std::vector<uint8_t>, std::string> readFile(const std::string& filename) const;

  /// @brief Writes a file to this device via File over EtherCAT (FoE).
  ///
  /// @param filename  FoE filename as recognised by the slave firmware.
  /// @param data      File bytes to write.
  /// @return Void on success, or an error string if the transfer fails.
  std::expected<void, std::string> writeFile(const std::string& filename,
                                             std::span<const uint8_t> data) const;

  /// @brief Reads bytes from an ESC register on this device.
  ///
  /// Delegates to the fieldbus driver's @c readRegister using this device's slave position.
  ///
  /// @param address  ESC register address (e.g. @c 0x0130 for DL Status).
  /// @param data     Output buffer; its size determines how many bytes are read.
  /// @return Void on success, or an error string on failure.
  std::expected<void, std::string> readRegister(uint16_t address, std::span<uint8_t> data) const;

  /// @brief Writes bytes to an ESC register on this device.
  ///
  /// Delegates to the fieldbus driver's @c writeRegister using this device's slave position.
  ///
  /// @param address  ESC register address.
  /// @param data     Bytes to write.
  /// @return Void on success, or an error string on failure.
  std::expected<void, std::string> writeRegister(uint16_t address,
                                                 std::span<const uint8_t> data) const;

  /// @brief Enumerates the device's CoE object dictionary and populates @c parameters().
  ///
  /// Requires the device to be in PRE-OP, SAFE-OP, or OP (mailbox communication
  /// active). One @c DeviceParameter is created per @c (index, subindex) pair returned
  /// by the SDO Info service, with @c value pre-initialised to a type-appropriate zero.
  /// When @p readValues is @c true each entry is additionally read via SDO upload and
  /// the decoded value stored on the parameter; entries that fail to read keep their
  /// default value and the call still succeeds (per-entry errors are logged).
  ///
  /// Calling this method again replaces the existing parameter map.
  ///
  /// @param readValues  When @c true, follow up each entry with an SDO upload.
  /// @return Void on success, or an error string if the object dictionary enumeration
  ///         itself fails (the slave does not support SDO Info, or all retries timed out).
  std::expected<void, std::string> initializeParameters(bool readValues = false);

  /// @brief Returns the parameter map, keyed by @c makeParameterKey(index, subindex).
  /// Empty until @c initializeParameters() is called.
  const std::unordered_map<uint32_t, DeviceParameter>& parameters() const;

  /// @brief Returns all parameters sorted ascending by @c (index, subindex).
  ///
  /// Copies the map into a vector and sorts on the packed key. O(N log N) — call
  /// when you need stable iteration order (e.g. JSON serialisation, UI listings)
  /// rather than O(1) lookup.
  std::vector<DeviceParameter> parametersOrdered() const;

  /// @brief Looks up a parameter by @c (index, subindex). O(1).
  /// @return Pointer to the parameter, or @c nullptr if no such entry exists.
  const DeviceParameter* parameter(uint16_t index, uint8_t subindex) const;

 private:
  uint16_t slavePosition_;
  mm::comm::FieldbusDriver& driver_;
  std::string name_;
  uint32_t vendorId_;
  uint32_t productCode_;
  uint32_t revisionNumber_;
  uint32_t serialNumber_;
  std::unordered_map<uint32_t, DeviceParameter> parameters_;
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
