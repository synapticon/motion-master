#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace mm::comm {

/// @brief Immutable identity fields read from a slave's EEPROM during configuration.
struct SlaveInfo {
  std::string name;         ///< Human-readable name from SII.
  uint32_t vendorId;        ///< Vendor ID (EEprom manufacturer field).
  uint32_t productCode;     ///< Product code (EEprom ID field).
  uint32_t revisionNumber;  ///< Revision number.
  uint32_t serialNumber;    ///< Serial number.
};

/// @brief Abstract interface for an EtherCAT fieldbus driver.
///
/// Concrete implementations: @c SoemFieldbusDriver (SOEM), @c SpoeDriver (SPoE).
/// @c App instantiates exactly one and injects it into @c DeviceManager and
/// @c GameLoop.
///
/// The driver owns the mutex that serialises all EtherCAT socket access — both
/// the real-time PDO path (@c exchangeProcessData, called from the RT thread)
/// and the SDO path (called from HTTP handler threads).
class FieldbusDriver {
 public:
  virtual ~FieldbusDriver() = default;

  /// @brief Opens the network interface and initialises the master context.
  ///
  /// Must be called before any other driver method.
  ///
  /// @return Void on success, or an error string describing the failure.
  virtual std::expected<void, std::string> init() = 0;

  /// @brief Discovers slaves and configures their sync managers and FMMUs.
  ///
  /// Must be called after a successful @c init(). Slaves remain in INIT state —
  /// state transitions are left entirely to the caller.
  ///
  /// @return Number of slaves found on success, or an error string if configuration fails.
  virtual std::expected<int, std::string> configure() = 0;

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
};

}  // namespace mm::comm
