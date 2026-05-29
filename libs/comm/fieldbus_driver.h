#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mm::comm {

/// @brief EtherCAT Application Layer state values.
///
/// Numeric values match the EtherCAT standard AL control/status register
/// encoding (ETG.1000.6 §6.4.1).
enum class EtherCatState : uint16_t {
  Init = 0x01,    ///< INIT — reset state, no communication.
  PreOp = 0x02,   ///< PRE-OPERATIONAL — mailbox communication active.
  Boot = 0x03,    ///< BOOT — firmware download mode.
  SafeOp = 0x04,  ///< SAFE-OPERATIONAL — inputs only; outputs ignored.
  Op = 0x08,      ///< OPERATIONAL — full PDO exchange.
};

/// @brief Immutable identity fields read from a slave's EEPROM during configuration.
struct SlaveInfo {
  std::string name;         ///< Human-readable name from SII.
  uint32_t vendorId;        ///< Vendor ID (EEprom manufacturer field).
  uint32_t productCode;     ///< Product code (EEprom ID field).
  uint32_t revisionNumber;  ///< Revision number.
  uint32_t serialNumber;    ///< Serial number.
};

/// @brief Schema of a single object dictionary entry uploaded from a slave.
///
/// Populated by @c FieldbusDriver::readObjectDictionary, one entry per @c (index,
/// subindex) pair. Holds the immutable description only — no current value.
///
/// @c defaultValue / @c minValue / @c maxValue carry the raw bytes returned by
/// the SDO Info "Get Entry Description" service when the slave populates them;
/// they remain empty when the driver does not request these flags or when the
/// slave does not support them. The raw byte form is intentional — decoding into
/// a typed variant is done at the @c node layer where @c dataType is interpreted.
struct OdEntry {
  uint16_t index;                ///< CoE object index.
  uint8_t subindex;              ///< CoE object subindex.
  uint16_t objectCode;           ///< OTYPE_VAR / OTYPE_ARRAY / OTYPE_RECORD (ETG.1000.6 §5).
  uint16_t dataType;             ///< ETG.1020 data type code (e.g. 0x0007 = UNSIGNED32).
  uint16_t bitLength;            ///< Bit length of the entry.
  uint16_t access;               ///< ObjAccess bitfield (read/write per AL state).
  std::string name;              ///< Textual description.
  std::optional<uint32_t> unit;  ///< ETG.1004 unit code, if available.
  std::optional<std::vector<uint8_t>> defaultValue;  ///< Raw default-value bytes, if available.
  std::optional<std::vector<uint8_t>> minValue;      ///< Raw minimum-value bytes, if available.
  std::optional<std::vector<uint8_t>> maxValue;      ///< Raw maximum-value bytes, if available.
};

/// @brief Abstract interface for an EtherCAT fieldbus driver.
///
/// Concrete implementations: @c SoemFieldbusDriver (SOEM), @c SpoeDriver (SPoE).
/// @c App instantiates exactly one and injects it into @c DeviceManager and
/// @c GameLoop.
///
/// The driver owns @c socketMutex_, which serialises the control-plane
/// operations (mailbox/SDO, FoE, ESC register, and state access) amongst
/// non-RT callers. The real-time PDO path (@c exchangeProcessData) runs
/// lock-free: SOEM's port layer is internally thread-safe (per-datagram index
/// allocation plus tx/rx mutexes held only for a single non-blocking poll), and
/// PDO touches disjoint state (the process-data IOmap) from the control plane
/// (mailbox pool, slave state). Keeping PDO out of the mutex keeps the RT cycle
/// unbounded by a slow SDO or object-dictionary enumeration.
class FieldbusDriver {
 public:
  virtual ~FieldbusDriver() = default;

  /// @brief Opens the network interface and initialises the master context.
  ///
  /// Must be called before any other driver method.
  ///
  /// @return Void on success, or an error string describing the failure.
  virtual std::expected<void, std::string> init() = 0;

  /// @brief Scans the bus for slaves and configures their sync managers and FMMUs.
  ///
  /// Must be called after a successful @c init(). Slaves remain in INIT state —
  /// state transitions are left entirely to the caller.
  ///
  /// @return Number of slaves found on success, or an error string if the scan fails.
  virtual std::expected<int, std::string> scan() = 0;

  /// @brief Returns the immutable identity fields for the slave at @p position.
  /// @param position  1-based slave position on the bus.
  virtual SlaveInfo slaveInfo(uint16_t position) const = 0;

  /// @brief Exchanges process data with all slaves in one EtherCAT LRW frame.
  ///
  /// Called once per @c GameLoop cycle.  Must complete within the cycle budget;
  /// timing jitter here propagates directly to control latency.  Must not be
  /// called before a successful @c init() or after @c stop().
  virtual void exchangeProcessData() = 0;

  /// @brief Transitions all slaves to INIT state and closes the network interface.
  ///
  /// After @c stop() returns, @c exchangeProcessData() must not be called again.
  virtual void stop() = 0;

  /// @brief AL Status and AL Status Code for a single slave.
  struct SlaveStateRaw {
    uint16_t alStatus;      ///< Raw AL Status register (bits 3:0 = state, bit 4 = error).
    uint16_t alStatusCode;  ///< AL Status Code register (ETG.1000.6 §6.4.1).
  };

  /// @brief Reads the current AL Status for each slave in @p positions.
  ///
  /// Refreshes slave state from the hardware in one pass, then returns the raw
  /// AL Status register and AL Status Code register for each requested position.
  /// AL Status bits 3:0 encode the current state (1=Init, 2=PreOp, 3=Boot,
  /// 4=SafeOp, 8=Op); bit 4 is the error indicator.  AL Status Code is non-zero
  /// when an error is present and identifies the cause (ETG.1000.6 §6.4.1).
  ///
  /// @param positions  1-based slave positions to read.
  /// @return Raw state per position in the same order as @p positions, or an
  ///         error string if the hardware read fails.
  virtual std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) = 0;

  /// @brief Reads an object dictionary entry via CoE SDO upload.
  ///
  /// Allocates up to 4096 bytes, performs a mailbox SDO upload, and returns the exact
  /// bytes the slave sent (resized to the actual transfer size).
  /// Called from HTTP handler threads; must not overlap with @c exchangeProcessData.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @param index          CoE object index.
  /// @param subindex       CoE object subindex.
  /// @return The bytes transferred on success, or an error string if the mailbox transfer fails.
  virtual std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t slavePosition,
                                                                   uint16_t index,
                                                                   uint8_t subindex) = 0;

  /// @brief Enumerates the entire CoE object dictionary of a slave via SDO Info.
  ///
  /// Performs the "Get Object List" → "Get Object Description" → "Get Entry Description"
  /// sequence (ETG.1000.6 §5.6) and returns one @c OdEntry per @c (index, subindex) pair.
  /// The slave must be in PRE-OP, SAFE-OP, or OP — i.e. mailbox communication enabled.
  /// Many transfers are issued; expect the call to take seconds on a fully populated drive.
  ///
  /// Slaves that do not implement the SDO Info service return an error.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @return All OD entries on success, or an error string if any phase of the enumeration
  ///         fails after the driver's internal retries.
  virtual std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(
      uint16_t slavePosition) = 0;

  /// @brief Reads a file from the slave via File over EtherCAT (FoE).
  ///
  /// Sends an FoE read request for @p filename and collects all data packets from the slave.
  /// FoE is available in Boot, Pre-Op, Safe-Op, and Op states (device-dependent); the caller is
  /// responsible for ensuring the device is in a suitable state.
  /// Called from HTTP handler threads; must not overlap with @c exchangeProcessData.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @param filename       FoE filename as recognised by the slave firmware.
  /// @return File bytes on success, or an error string if the transfer fails.
  virtual std::expected<std::vector<uint8_t>, std::string> readFile(
      uint16_t slavePosition, const std::string& filename) = 0;

  /// @brief Writes a file to the slave via File over EtherCAT (FoE).
  ///
  /// Sends an FoE write request for @p filename and streams @p data to the slave.
  /// FoE is available in Boot, Pre-Op, Safe-Op, and Op states (device-dependent); the caller is
  /// responsible for ensuring the device is in a suitable state.
  /// Called from non-RT (HTTP handler) threads; serialized with other control-plane
  /// operations via the driver's socket mutex.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @param filename       FoE filename as recognised by the slave firmware.
  /// @param data           File bytes to write.
  /// @return Void on success, or an error string if the transfer fails.
  virtual std::expected<void, std::string> writeFile(uint16_t slavePosition,
                                                     const std::string& filename,
                                                     std::span<const uint8_t> data) = 0;

  /// @brief Reads bytes from an ESC register via a Configured-Address Read (FPRD) datagram.
  ///
  /// @p data.size() bytes are read from register @p address of the slave at @p slavePosition.
  /// Called from HTTP handler threads; must not overlap with @c exchangeProcessData.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @param address        ESC register address (e.g. @c 0x0130 for DL Status).
  /// @param data           Output buffer; its size determines how many bytes are read.
  /// @return Void on success, or an error string if no slave responded.
  virtual std::expected<void, std::string> readRegister(uint16_t slavePosition, uint16_t address,
                                                        std::span<uint8_t> data) = 0;

  /// @brief Writes bytes to an ESC register via a Configured-Address Write (FPWR) datagram.
  ///
  /// @p data.size() bytes are written to register @p address of the slave at @p slavePosition.
  /// Called from HTTP handler threads; must not overlap with @c exchangeProcessData.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @param address        ESC register address.
  /// @param data           Bytes to write.
  /// @return Void on success, or an error string if no slave responded.
  virtual std::expected<void, std::string> writeRegister(uint16_t slavePosition, uint16_t address,
                                                         std::span<const uint8_t> data) = 0;

  /// @brief Commands a set of devices to @p targetState and blocks until all arrive or
  ///        @p timeout elapses.
  ///
  /// Devices whose current state (error bit masked) does not match @p requiredState are
  /// skipped; pass @c std::nullopt to command all @p positions unconditionally.
  ///
  /// The call polls at ~100 ms intervals, re-sending the command to lagging devices every
  /// @p resendInterval.  Devices that do not arrive in time are logged at error level; no
  /// exception is thrown.  The outcome per device is not returned here — read it back via
  /// @c readStates after the call (this is what @c DeviceManager::transitionToState does).
  ///
  /// @param positions       1-based device positions to target.
  /// @param requiredState   Pre-filter: only command devices whose current state equals this
  ///                        value.  @c std::nullopt skips filtering and commands all positions.
  /// @param targetState     Desired state.
  /// @param timeout         Maximum time to wait for all devices.
  /// @param resendInterval  How often to re-send the command to lagging devices.
  /// @param tick            Optional callback invoked at ~1 ms intervals while waiting.
  ///                        Pass a PDO sender when targeting @c EtherCatState::Op so the
  ///                        sync-manager watchdog does not fire during the wait.
  /// @param shouldAbort     Optional predicate; when it returns @c true the wait is abandoned
  ///                        early without logging failures for pending devices.
  virtual void transitionToState(
      const std::vector<uint16_t>& positions, std::optional<EtherCatState> requiredState,
      EtherCatState targetState, std::chrono::steady_clock::duration timeout,
      std::chrono::steady_clock::duration resendInterval = std::chrono::seconds(2),
      std::function<void()> tick = nullptr, std::function<bool()> shouldAbort = nullptr) = 0;

 protected:
  /// Serialises control-plane access to the underlying fieldbus context (mailbox
  /// pool, slave state, manual mailbox sequence counters) amongst non-RT callers.
  /// Held only for the duration of a single socket transaction — never across a
  /// sleep, a blocking wait, or a user callback. The PDO path does not take this
  /// lock (see the class-level note). @c mutable so the @c const accessors
  /// (@c slaveInfo, @c slaveCount) can lock.
  mutable std::mutex socketMutex_;
};

}  // namespace mm::comm
