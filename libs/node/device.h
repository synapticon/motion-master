#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "node/device_parameter.h"
#include "node/pdo_mapping.h"

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

  /// @brief Whether the device currently has an active SDO mailbox (PRE-OP, SAFE-OP, or OP,
  ///        error bit clear).
  ///
  /// Derived live from the fieldbus driver's cached AL status (@c FieldbusDriver::slaveState)
  /// — no copy is stored here. When @c false the device is treated as offline:
  /// @c readParameter / @c writeParameter operate on the cached value only and never touch
  /// the bus. Reflects the last state the driver read; call @c DeviceManager::getDeviceStates
  /// to refresh from the hardware.
  bool online() const;

  /// @brief Whether the device is in a process-data-exchanging state (SAFE-OP or OP, error
  ///        bit clear).
  ///
  /// Derived live from the driver's cached AL status. When @c false (INIT / PRE-OP / BOOT)
  /// the device does not participate in the LRW cycle, so its region of the process image is
  /// stale — PDO-mapped parameter access must use SDO, not the shared buffers. Lets a
  /// partially-operational bus route each device correctly.
  bool exchangesProcessData() const;

  /// @brief Uploads an object dictionary entry from the device (CoE SDO upload).
  ///
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @return The bytes transferred on success, or an error string if the mailbox transfer fails.
  std::expected<std::vector<uint8_t>, std::string> upload(uint16_t index, uint8_t subindex) const;

  /// @brief Downloads (writes) an object dictionary entry to the device (CoE SDO download).
  ///
  /// Requires the device to be in PRE-OP, SAFE-OP, or OP (mailbox communication active).
  ///
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @param data      Bytes to write; size must match the object's length.
  /// @return Void on success, or an error string if the mailbox transfer fails.
  std::expected<void, std::string> download(uint16_t index, uint8_t subindex,
                                            std::span<const uint8_t> data) const;

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

  /// @brief Reads the device's PDO mapping from its assignment and mapping objects.
  ///
  /// Walks the PDO assignment objects (@c 0x1C12 for outputs/RxPDO, @c 0x1C13 for
  /// inputs/TxPDO) and the mapping objects they reference (@c 0x16xx / @c 0x1Axx) via SDO
  /// upload, producing one @c PdoMappingEntry per mapped object with its bit offset and
  /// width within the slave's process-data window.  Requires the device to be in PRE-OP,
  /// SAFE-OP, or OP (mailbox communication active).
  ///
  /// A direction whose assignment object is absent or assigns nothing yields an empty list
  /// for that direction (the device simply has no PDOs in it) and is not an error.  Calling
  /// this again replaces the existing mapping.
  ///
  /// @return Void on success, or an error string if a mapping object referenced by an
  ///         assignment cannot be read (an inconsistent mapping the caller must not exchange).
  std::expected<void, std::string> readPdoMappings();

  /// @brief Returns the device's PDO mapping. Empty until @c readPdoMappings() succeeds.
  const PdoMappings& pdoMappings() const;

  /// @brief Stores a parameter's value locally, without any bus access (the typed setter).
  ///
  /// Coerces @p value into the parameter's declared type, stores it, and marks it
  /// @c SyncState::Synced.  Used to reflect a value obtained out-of-band — e.g. one decoded
  /// from the process image by @c DeviceManager — so @c DeviceParameter stays the source of
  /// truth.  The parameter must already exist.  Unlike @c writeParameter this never touches the
  /// wire; it is the typed counterpart of @c setValueFromBytes.
  ///
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @param value     Value to store; coerced to the parameter's declared type.
  /// @return Void on success, or an error string if the parameter is unknown or @p value
  ///         cannot be coerced to its type.
  std::expected<void, std::string> setValue(uint16_t index, uint8_t subindex,
                                            DeviceParameterValue value);

  /// @brief Sets the parameter's value from its raw on-the-wire bytes (the bytes-domain setter).
  ///
  /// The byte-input counterpart of @c setValue: decodes @p bytes with the parameter's
  /// declared data type, stores the result (marking it @c SyncState::Synced), and returns the
  /// decoded value — all under @c parametersMutex_ so the data-type lookup, decode, and store are
  /// one atomic step. @p bytes are the LSB-aligned little-endian encoding of the object's value;
  /// the source is irrelevant (a slice of the process image, an SDO upload, a test fixture).
  ///
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @param bytes     Raw LSB-aligned little-endian value bytes.
  /// @return The decoded value, or an error string if the parameter is unknown or decoding fails.
  std::expected<DeviceParameterValue, std::string> setValueFromBytes(
      uint16_t index, uint8_t subindex, std::span<const uint8_t> bytes);

  /// @brief Returns the parameter's value as its raw on-the-wire bytes (the bytes-domain getter).
  ///
  /// Reads the stored value and its declared data type under @c parametersMutex_ and encodes them,
  /// so a caller can stage the current setpoint into the output image without reaching into the
  /// parameter map itself. The encode counterpart of @c setValueFromBytes.
  ///
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @return The encoded bytes, or an error string if the parameter is unknown or encoding fails.
  std::expected<std::vector<uint8_t>, std::string> valueAsBytes(uint16_t index,
                                                                uint8_t subindex) const;

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
  ///
  /// @warning Returns a raw pointer into the parameter map; not synchronised against the cache
  /// lock. Call on the control-plane thread only. Off-thread readers use the thread-safe
  /// @c value / @c dataType getters below, which return copies under the lock.
  const DeviceParameter* parameter(uint16_t index, uint8_t subindex) const;

  /// @brief Returns a copy of a parameter's cached value (the typed cache getter), no bus access.
  ///
  /// The read counterpart of @c setValue, and thread-safe (taken under the cache lock) so the
  /// monitoring sampler can read it concurrently with refresher/control-plane writes. Returns
  /// the last value stored by a read/refresh; it does not itself touch the bus.
  ///
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @return The cached value, or @c nullopt if the parameter is unknown.
  std::optional<DeviceParameterValue> value(uint16_t index, uint8_t subindex) const;

  /// @brief Returns a parameter's declared ETG.1020 data-type code, thread-safely (cache lock).
  ///
  /// The data type is immutable once @c initializeParameters has populated the entry, but the
  /// map itself can be rebuilt off the caller's thread, so reading it under the lock is the safe
  /// way for an off-thread consumer (e.g. capturing a PDO decode spec) to obtain it.
  ///
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @return The data-type code, or @c nullopt if the parameter is unknown.
  std::optional<uint16_t> dataType(uint16_t index, uint8_t subindex) const;

  /// @brief Reads a parameter value, keeping the cached store in sync.
  ///
  /// When @c online(), uploads via SDO, decodes, stores the value (marking it
  /// @c SyncState::Synced) and returns it. When offline, returns the cached value
  /// without touching the bus. The parameter must already exist in the map (populated
  /// by @c initializeParameters).
  ///
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @return The (possibly freshly read) value, or an error string if the parameter is
  ///         unknown or, when online, the SDO upload / decode fails.
  std::expected<DeviceParameterValue, std::string> readParameter(uint16_t index, uint8_t subindex);

  /// @brief Writes a parameter value, always updating the cache first.
  ///
  /// @p value is coerced into the parameter's declared data type and stored in the cache
  /// (the cache is the source of truth). Then:
  /// - online: the value is encoded and downloaded via SDO. On success the parameter is
  ///   marked @c SyncState::Synced; on download failure it is marked @c SyncState::Pending
  ///   and the error is returned.
  /// - offline: the parameter is marked @c SyncState::Pending and the call succeeds — the
  ///   edit lives in the cache until the device comes back online and it is re-written.
  ///
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @param value     Value to set; coerced to the parameter's type.
  /// @return Void on success (including the offline cache-only case), or an error string
  ///         if the parameter is unknown, @p value cannot be coerced, or an online
  ///         download fails.
  std::expected<void, std::string> writeParameter(uint16_t index, uint8_t subindex,
                                                  DeviceParameterValue value);

  /// @brief Typed convenience wrapper for @c writeParameter.
  ///
  /// Lets callers pass a bare value without constructing a @c DeviceParameterValue —
  /// e.g. @c device.writeValue(0x2030, 1, 123). @p value is coerced into the
  /// parameter's declared type, so the literal's own type need not match the object's
  /// width. Online / offline behaviour is exactly that of @c writeParameter.
  ///
  /// @tparam T        Any type a @c DeviceParameterValue can hold (integers, floats,
  ///                  @c std::string, @c std::vector<uint8_t>).
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @param value     Value to write; coerced to the parameter's type.
  /// @return Void on success (including the offline cache-only case), or an error string.
  template <typename T>
  std::expected<void, std::string> writeValue(uint16_t index, uint8_t subindex, T value) {
    return writeParameter(index, subindex, DeviceParameterValue{value});
  }

  /// @brief Typed convenience wrapper for @c readParameter.
  ///
  /// Refreshes the value from the device when online (otherwise serves the cache) and
  /// returns it as @p T — e.g. @c device.readValue<int32_t>(0x6064, 0). The request is
  /// type-exact; use @c parameter(index, subindex)->numeric() when you only need a number.
  ///
  /// @tparam T        The expected variant alternative.
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @return The value as @p T, or an error string if the parameter is unknown, the read
  ///         fails, or the stored value is not a @p T.
  template <typename T>
  std::expected<T, std::string> readValue(uint16_t index, uint8_t subindex) {
    auto v = readParameter(index, subindex);
    if (!v) {
      return std::unexpected(v.error());
    }
    if (const auto* p = std::get_if<T>(&*v)) {
      return *p;
    }
    return std::unexpected(
        std::format("parameter 0x{:04X}:{:02X} holds a different type", index, subindex));
  }

 private:
  /// @brief Mutable parameter lookup by @c (index, subindex). O(1); @c nullptr if absent.
  DeviceParameter* findParameter(uint16_t index, uint8_t subindex);

  /// @brief Reads one PDO direction: the assignment object and the mapping objects it
  ///        references, appending entries to @p out and accumulating the bit offset.
  ///
  /// @param assignmentIndex  @c 0x1C12 (outputs/RxPDO) or @c 0x1C13 (inputs/TxPDO).
  /// @param out              Destination entry list (cleared first).
  /// @param totalBits        Set to the total mapped width across all entries.
  /// @return Void on success, or an error string if a referenced mapping object fails to read.
  std::expected<void, std::string> readPdoAssignment(uint16_t assignmentIndex,
                                                     std::vector<PdoMappingEntry>& out,
                                                     uint32_t& totalBits);

  uint16_t slavePosition_;
  mm::comm::FieldbusDriver& driver_;
  std::string name_;
  uint32_t vendorId_;
  uint32_t productCode_;
  uint32_t revisionNumber_;
  uint32_t serialNumber_;
  // Guards parameters_ against the off-RT monitoring threads (the refresher refreshes cached
  // values, the sampler reads them) racing the control-plane thread. Held only briefly — across
  // a cache read/write, or a single mailbox transaction in read/writeParameter; never across the
  // multi-entry object-dictionary enumeration, which builds a local map and swaps it in under
  // the lock. Lock order, where both are taken: DeviceManager::busMutex_ before this.
  //
  // Held by unique_ptr because std::mutex is neither movable nor copyable, and Device is moved
  // into DeviceManager's std::vector<Device> (which relocates on growth). The indirection keeps
  // Device move-constructible (the pointer moves); a Device is never copied, only moved.
  std::unique_ptr<std::mutex> parametersMutex_;
  std::unordered_map<uint32_t, DeviceParameter> parameters_;
  PdoMappings pdoMappings_;
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

/// @brief Reconciles a device's Configured Module Ident List with its Detected list.
///
/// Walks the EtherCAT Modular Device Profile objects (ETG.5001): for every populated
/// slot in the Detected Module Ident List (@c 0xF050) it writes the detected ident into
/// the matching subindex of the Configured Module Ident List (@c 0xF030). A drive whose
/// configured and detected lists disagree reports a module mismatch and refuses to leave
/// PRE-OP; copying detected into configured clears it.
///
/// The device must be in PRE-OP or higher (SDO mailbox active) — @c 0xF030 is writable
/// in PRE-OP. Vendor-neutral: keyed only on the standard MDP objects, never on vendor ID.
/// Best-effort and idempotent: a device that does not implement @c 0xF050 (i.e. is not
/// modular) is treated as success with zero slots written, and slots whose configured
/// entry already matches the detected one are left untouched.
///
/// @param device  Target device; must have mailbox communication active (PRE-OP+).
/// @return The number of configured-list subindices written, or an error string naming
///         the slot(s) whose write to @c 0xF030 failed.
std::expected<int, std::string> reconcileDetectedModules(const Device& device);

}  // namespace mm::node
