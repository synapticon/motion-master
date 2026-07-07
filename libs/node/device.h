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

/// @brief The live process-data runtime (image + recorder ring + staging). Forward-declared here
///        and held by pointer so device.h stays free of its heavy buffers; the definition
///        (node/process_data.h) is pulled in only by device.cc, which calls readPdo / writePdo.
struct ProcessData;

/// @brief On-disk cache of CoE parameter definitions, keyed by device identity. Forward-declared
///        and held by pointer; the definition (node/parameter_cache.h) is pulled in only by
///        device.cc, which consults it from @c initializeParameters. Owned by @c DeviceManager.
class ParameterCache;

/// @brief Represents a single node on the fieldbus.
///
/// Holds the node's bus position, immutable identity read from EEPROM,
/// and a reference to the fieldbus driver for SDO and state operations.
class Device {
 public:
  /// @brief Constructs a device, reading identity from the driver at @p slavePosition.
  /// @param slavePosition  1-based position on the fieldbus (0 is reserved for the master).
  /// @param driver         Fieldbus driver; lifetime must exceed that of this object.
  /// @param processData    Live process-data runtime, or @c nullptr for SDO-only operation. When
  ///                       supplied (by @c DeviceManager), @c readParameter / @c writeParameter
  ///                       prefer the live PDO image while the device is exchanging and fall back
  ///                       to SDO otherwise. Lifetime must exceed that of this object.
  /// @param parameterCache On-disk cache of object-dictionary definitions, or @c nullptr to always
  ///                       enumerate live. When supplied (by @c DeviceManager),
  ///                       @c initializeParameters loads definitions from it on a hit and populates
  ///                       it on a miss. Lifetime must exceed that of this object.
  Device(uint16_t slavePosition, mm::comm::FieldbusDriver& driver,
         ProcessData* processData = nullptr, const ParameterCache* parameterCache = nullptr);

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

  /// @brief Whether the device's CoE/SDO mailbox is currently active (AL state PRE-OP,
  ///        SAFE-OP, or OP).
  ///
  /// Mailbox communication is available in PRE-OP and above per the EtherCAT state machine,
  /// independent of the AL error indicator — a device in SAFE-OP+error still answers mailbox
  /// requests. INIT has no mailbox and BOOT's is FoE-only, so both report @c false. Derived
  /// live from the fieldbus driver's cached AL status (@c FieldbusDriver::slaveState) — no
  /// copy is stored here. When @c false, @c readParameter / @c writeParameter operate on the
  /// cached value only and never touch the bus. Reflects the last state the driver read; call
  /// @c DeviceManager::getDeviceStates to refresh from the hardware.
  bool mailboxActive() const;

  /// @brief Whether the device is in a process-data-exchanging state (SAFE-OP or OP, error
  ///        bit clear).
  ///
  /// Derived live from the driver's cached AL status. When @c false (INIT / PRE-OP / BOOT)
  /// the device does not participate in the LRW cycle, so its region of the process image is
  /// stale — PDO-mapped parameter access must use SDO, not the shared buffers. Lets a
  /// partially-operational bus route each device correctly.
  bool exchangesProcessData() const;

  /// @brief Reads an object dictionary entry from the device (CoE SDO upload).
  ///
  /// Raw SDO read with no cache or PDO awareness — for the typed, PDO-aware path use
  /// @c readParameter instead.
  ///
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @return The bytes transferred on success, or an error string if the mailbox transfer fails.
  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t index, uint8_t subindex) const;

  /// @brief Writes an object dictionary entry to the device (CoE SDO download).
  ///
  /// Raw SDO write with no cache or PDO awareness — for the typed, PDO-aware path use
  /// @c writeParameter instead. Requires the device to be in PRE-OP, SAFE-OP, or OP (mailbox
  /// communication active).
  ///
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @param data      Bytes to write; size must match the object's length.
  /// @return Void on success, or an error string if the mailbox transfer fails.
  std::expected<void, std::string> writeSdo(uint16_t index, uint8_t subindex,
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

  /// @brief Reads this device's raw Slave Information Interface (SII / EEPROM) image.
  ///
  /// Delegates to the fieldbus driver's @c readSii using this device's slave position. Decode the
  /// returned bytes with @c mm::comm::parseSii. EEPROM access is most reliable while the device is
  /// in INIT or PRE-OP.
  ///
  /// @return The raw SII image on success, or an error string on failure.
  std::expected<std::vector<uint8_t>, std::string> readSii() const;

  /// @brief Writes a raw SII (EEPROM) image to this device.
  ///
  /// Delegates to the fieldbus driver's @c writeSii using this device's slave position.
  /// Destructive: a malformed image can leave the device unidentifiable until re-flashed. The
  /// device adopts the new contents only after a power cycle. Most reliable while the device is in
  /// INIT or PRE-OP.
  ///
  /// @param data  Raw SII image to write (even length).
  /// @return Void on success, or an error string on failure.
  std::expected<void, std::string> writeSii(std::span<const uint8_t> data) const;

  /// @brief Enumerates the device's CoE object dictionary and populates @c parameters().
  ///
  /// Requires the device to be in PRE-OP, SAFE-OP, or OP (mailbox communication
  /// active). One @c DeviceParameter is created per @c (index, subindex) pair returned
  /// by the SDO Info service, with @c value pre-initialised to a type-appropriate zero.
  /// When @p readValues is @c true each entry is additionally read and the decoded value stored on
  /// the parameter; entries that fail to read keep their default value and the call still succeeds
  /// (per-entry errors are logged).
  ///
  /// When @p useCompleteAccess is @c true, multi-subindex objects (ARRAY/RECORD) are read with a
  /// single CoE Complete Access upload instead of one upload per subindex — far fewer mailbox
  /// round-trips. Support is probed once: if the slave rejects the first CA read, the whole pass
  /// falls back to per-subindex reads. It has no effect unless @p readValues is @c true.
  ///
  /// Calling this method again replaces the existing parameter map.
  ///
  /// @param readValues        When @c true, follow up each entry with an SDO upload.
  /// @param useCompleteAccess  When @c true, use CoE Complete Access for multi-subindex objects.
  /// @return Void on success, or an error string if the object dictionary enumeration
  ///         itself fails (the slave does not support SDO Info, or all retries timed out).
  std::expected<void, std::string> initializeParameters(bool readValues = false,
                                                        bool useCompleteAccess = true);

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
  std::expected<void, std::string> readFlatPdoMapping();

  /// @brief Returns the device's PDO mapping. Empty until @c readFlatPdoMapping() succeeds.
  const FlatPdoMapping& flatPdoMapping() const;

  /// @brief Reads the device's PDO mapping grouped by mapping object (@c 0x16xx / @c 0x1Axx).
  ///
  /// The grouped counterpart of @c readFlatPdoMapping: instead of one flat list per direction, each
  /// mapping object keeps its @c pdoIndex and its own entries (with derived @c bitOffset), so the
  /// result round-trips into @c writePdoMapping. Reads fresh over SDO; does not touch the cached
  /// @c flatPdoMapping(). Requires the device to be in PRE-OP, SAFE-OP, or OP (mailbox active).
  ///
  /// @return The grouped mapping, or an error string if a referenced mapping object cannot be read.
  std::expected<PdoMapping, std::string> readPdoMapping();

  /// @brief Writes a new PDO mapping to the device via SDO, then reads it back to verify.
  ///
  /// Reconfigures both directions' sync-manager PDO assignment (@c 0x1C12 outputs / @c 0x1C13
  /// inputs) and the mapping objects (@c 0x16xx / @c 0x1Axx) they reference, following the CoE
  /// ordering rule (ETG.1000.6 §5.6.7.4.9): a sync manager's PDO assignment is cleared to zero
  /// (which makes its mapping objects writable), each mapping object's entry count is cleared, its
  /// entries are written as packed @c uint32 words (@c index<<16 | subindex<<8 | bitLength), the
  /// entry count is restored, and finally the assignment lists the mapping objects and its own
  /// count is written. A mapping object present in the previous configuration but absent from
  /// @p mapping is simply left unassigned — its contents are irrelevant once it is off the sync
  /// manager, so it needs no explicit clear.
  ///
  /// **Requires the device to be in PRE-OP.** The mapping and assignment objects are writable only
  /// in PRE-OP — INIT/BOOT have no CoE mailbox, and in SAFE-OP/OP the sync managers are active and
  /// the slave aborts the write. This is the "drop to PRE-OP, remap, climb back" flow: the caller
  /// takes the device to PRE-OP, calls this, then transitions it back to SAFE-OP/OP, at which point
  /// @c DeviceManager re-reads the mapping and rebuilds the whole-bus process image.
  ///
  /// After writing, the mapping is read back (via @c readFlatPdoMapping, which also refreshes
  /// @c flatPdoMapping()) and compared against @p mapping; a mismatch, or a transient SDO failure
  /// mid-sequence, is retried up to a small fixed number of whole-mapping attempts before failing,
  /// because a single dropped mailbox frame would otherwise leave the object dictionary
  /// half-configured. The apply is idempotent, so a retry is safe.
  ///
  /// @param mapping  The desired output (RxPDO) and input (TxPDO) mapping objects, in assignment
  ///                 order. An empty direction clears that sync manager's assignment.
  /// @return Void on success (the device's mapping matches @p mapping), or an error string if the
  ///         device is not in PRE-OP, an entry is malformed (bit length or count out of range), an
  ///         SDO write/read-back fails after all retries, or the read-back does not match.
  std::expected<void, std::string> writePdoMapping(const PdoMapping& mapping);

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
                                            DeviceParameterValue newValue);

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

  /// @brief Returns a copy of a full parameter struct (value + metadata), no bus access.
  ///
  /// The struct-level analogue of @c value: a deep copy taken under the cache lock, so it is safe
  /// to call off the control-plane thread (unlike @c parameter, which hands out a raw pointer).
  /// Reflects the last value stored by a read/refresh; it does not itself touch the bus.
  ///
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @return A copy of the parameter, or @c nullopt if the parameter is unknown.
  std::optional<DeviceParameter> parameterCopy(uint16_t index, uint8_t subindex) const;

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
  /// Routing, in order:
  /// - When the device is exchanging (SAFE-OP/OP) and process-image access was injected, the
  ///   live PDO value is taken from the process image (if the object is PDO-mapped and the bus is
  ///   healthy), decoded, stored (marking it @c SyncState::Synced) and returned — no bus I/O.
  /// - Otherwise, when @c mailboxActive(), uploads via SDO, decodes, stores and returns it.
  /// - Otherwise (no mailbox) returns the cached value without touching the bus.
  ///
  /// The parameter must already exist in the map (populated by @c initializeParameters).
  ///
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @return The (possibly freshly read) value, or an error string if the parameter is
  ///         unknown or, when online, the SDO upload / decode fails.
  std::expected<DeviceParameterValue, std::string> readParameter(uint16_t index, uint8_t subindex);

  /// @brief Refreshes the cached value of every readable parameter, keeping the list intact.
  ///
  /// Re-reads each entry already in the map (it does not re-enumerate the object dictionary —
  /// use @c initializeParameters for that). PDO-mapped objects are read from the live process
  /// image when exchanging; everything else is read over the mailbox — as one CoE Complete Access
  /// upload per multi-subindex ARRAY/RECORD when @p useCompleteAccess is @c true (probed once, with
  /// per-object fallback), or one SDO upload per subindex otherwise. The objects are snapshotted
  /// under the lock first, then read with the lock released between objects (each transfer holds it
  /// for one round-trip), so a concurrent cached read of this device never waits for the whole —
  /// potentially multi-second — sweep.
  ///
  /// Write-only objects are skipped (an SDO upload of one would abort). Best-effort, like
  /// @c initializeParameters(readValues=true): an entry that fails to read keeps its cached value
  /// and is logged, and the call still succeeds so one bad object never blocks the rest.
  ///
  /// @param useCompleteAccess  When @c true, use CoE Complete Access for multi-subindex objects
  ///                           read over the mailbox.
  /// @return Void on success (the always-taken best-effort path), or an error string if the
  ///         device has no parameters loaded yet (call @c initializeParameters first).
  std::expected<void, std::string> readAllParameters(bool useCompleteAccess = true);

  /// @brief Writes a parameter value, always updating the cache first.
  ///
  /// @p value is coerced into the parameter's declared data type and stored in the cache
  /// (the cache is the source of truth). Then, in order:
  /// - exchanging (SAFE-OP/OP) with process-image access injected and the object output-mapped:
  ///   the value is staged into the output image (sent next cycle), marked @c SyncState::Synced,
  ///   and the call returns — no SDO download.
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
                                                  DeviceParameterValue newValue);

  /// @brief Typed convenience wrapper for @c writeParameter.
  ///
  /// Lets callers pass a bare value without constructing a @c DeviceParameterValue —
  /// e.g. @c device.writeValue(0x2030, 1, 123). @p newValue is coerced into the
  /// parameter's declared type, so the literal's own type need not match the object's
  /// width. Online / offline behaviour is exactly that of @c writeParameter.
  ///
  /// @tparam T        Any type a @c DeviceParameterValue can hold (integers, floats,
  ///                  @c std::string, @c std::vector<uint8_t>).
  /// @param index     CoE object index.
  /// @param subindex  CoE object subindex.
  /// @param newValue  Value to write; coerced to the parameter's type.
  /// @return Void on success (including the offline cache-only case), or an error string.
  template <typename T>
  std::expected<void, std::string> writeValue(uint16_t index, uint8_t subindex, T newValue) {
    return writeParameter(index, subindex, DeviceParameterValue{newValue});
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

  /// @brief Fills in live values on @p defs (the value-read pass of @c initializeParameters).
  ///
  /// Reads each object over CoE and stores the decoded value on its entries. When
  /// @p useCompleteAccess is @c true, multi-subindex ARRAY/RECORD objects are read with a single
  /// Complete Access upload (support probed once, per-object fallback); everything else — and any
  /// object that is not CA-decodable or whose CA read fails — is read one subindex at a time. Runs
  /// off the lock and mutates @p defs in place; the caller publishes the built map under
  /// @c parametersMutex_. Per-entry failures are logged and leave the type default.
  void readParameterValues(std::vector<DeviceParameter>& defs, bool useCompleteAccess);

  /// @brief Reads one PDO direction grouped by mapping object: the assignment object and each
  ///        mapping object it references, with every entry's @c bitOffset derived from the running
  ///        offset across the direction. The shared reader behind @c readPdoMapping and (flattened)
  ///        @c readFlatPdoMapping.
  ///
  /// @param assignmentIndex  @c 0x1C12 (outputs/RxPDO) or @c 0x1C13 (inputs/TxPDO).
  /// @return The mapping objects in assignment order (empty if the direction assigns nothing), or
  ///         an error string if a referenced mapping object fails to read.
  std::expected<std::vector<PdoMappingObject>, std::string> readPdoAssignment(
      uint16_t assignmentIndex);

  uint16_t slavePosition_;
  mm::comm::FieldbusDriver& driver_;
  // Live process-data runtime, or nullptr for SDO-only operation. Injected by DeviceManager so
  // read/writeParameter can prefer PDO over SDO while exchanging. Non-owning; the owner
  // (DeviceManager) outlives every Device it created. A raw pointer keeps Device
  // move-constructible.
  ProcessData* processData_ = nullptr;
  // On-disk parameter-definition cache, or nullptr to always enumerate live. Non-owning; owned by
  // DeviceManager, which outlives every Device. Consulted only by initializeParameters.
  const ParameterCache* parameterCache_ = nullptr;
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
  FlatPdoMapping flatPdoMapping_;
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
