#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "comm/fieldbus_driver.h"

struct ecx_context;

namespace mm::comm::soem {

/// @brief SOEM-backed EtherCAT fieldbus driver.
///
/// Owns one @c ecx_contextt master context and its PDO I/O map.  @c App
/// creates exactly one instance and injects it into @c DeviceManager and
/// @c GameLoop.
class SoemFieldbusDriver : public FieldbusDriver {
 public:
  /// @brief Constructs the driver for the given network interface.
  /// @param ifname  OS network interface name (e.g. @c "eth0", @c "enp3s0").
  explicit SoemFieldbusDriver(std::string ifname);

  /// @brief Closes the NIC if @c init() succeeded.
  ~SoemFieldbusDriver() override;

  SoemFieldbusDriver(const SoemFieldbusDriver&) = delete;
  SoemFieldbusDriver& operator=(const SoemFieldbusDriver&) = delete;

  /// @brief Opens the NIC and initialises the SOEM master context.
  ///
  /// Must be called before any other driver method.  Device discovery and state
  /// transitions are performed by separate functions after a successful init.
  ///
  /// @return Void on success, or an error string if the interface cannot be opened.
  std::expected<void, std::string> init() override;

  /// @brief Scans the bus for slaves and configures their sync managers and FMMUs.
  ///
  /// Sets @c manualstatechange so slaves remain in INIT after the scan —
  /// state transitions are left to the caller.
  ///
  /// @return Number of slaves found on success, or an error string if no slaves are found.
  std::expected<int, std::string> scan() override;

  /// @brief Returns the immutable identity fields for the slave at @p position.
  /// @param position  1-based slave position on the bus.
  SlaveInfo slaveInfo(uint16_t position) const override;

  /// @brief Sends output PDOs and receives input PDOs in one LRW frame.
  ///
  /// Called once per @c GameLoop cycle.  Must not be called before a successful
  /// @c init() or after @c stop().
  void exchangeProcessData() override;

  /// @brief Closes the NIC and releases all driver resources.
  void stop() override;

  /// @copydoc FieldbusDriver::readStates
  std::expected<std::vector<FieldbusDriver::SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override;

  /// @copydoc FieldbusDriver::readSdo
  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t slavePosition, uint16_t index,
                                                           uint8_t subindex) override;

  /// @copydoc FieldbusDriver::readFile
  std::expected<std::vector<uint8_t>, std::string> readFile(uint16_t slavePosition,
                                                            const std::string& filename) override;

  /// @brief Reads bytes from an ESC register via FPRD.
  /// @param slavePosition  1-based slave position on the bus.
  /// @param address        ESC register address.
  /// @param data           Output buffer; its size determines how many bytes are read.
  /// @return Void on success, or an error string if the working counter is not 1.
  std::expected<void, std::string> readRegister(uint16_t slavePosition, uint16_t address,
                                                std::span<uint8_t> data) override;

  /// @brief Writes bytes to an ESC register via FPWR.
  /// @param slavePosition  1-based slave position on the bus.
  /// @param address        ESC register address.
  /// @param data           Bytes to write.
  /// @return Void on success, or an error string if the working counter is not 1.
  std::expected<void, std::string> writeRegister(uint16_t slavePosition, uint16_t address,
                                                 std::span<const uint8_t> data) override;

  /// @brief Returns the number of discovered slaves, or 0 before discovery.
  int slaveCount() const;

  /// @copydoc FieldbusDriver::transitionToState
  void transitionToState(
      const std::vector<uint16_t>& positions, std::optional<EtherCatState> requiredState,
      EtherCatState targetState, std::chrono::steady_clock::duration timeout,
      std::chrono::steady_clock::duration resendInterval = std::chrono::seconds(2),
      std::function<void()> tick = nullptr, std::function<bool()> shouldAbort = nullptr) override;

 private:
  std::string ifname_;
  // ecx_contextt is several hundred KB (EC_MAXSLAVE slave entries) — heap-
  // allocated and null until init() succeeds.
  std::unique_ptr<ecx_context> ctx_;
  uint8_t map_[4096]{};
};

}  // namespace mm::comm::soem
