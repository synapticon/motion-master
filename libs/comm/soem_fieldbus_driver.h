#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "comm/fieldbus_driver.h"

struct ecx_context;

namespace mm::comm::soem {

/// @brief SOEM-backed fieldbus driver.
///
/// Owns one EtherCAT master context.  Constructed with the name of the
/// network interface to use (e.g. "eth0").  App creates exactly one instance
/// and injects it into DeviceManager, GameLoop, SdoService, etc.
class SoemFieldbusDriver : public IFieldbusDriver {
 public:
  /// @brief Constructs the driver for the given network interface.
  /// @param ifname  OS network interface name (e.g. "eth0", "enp3s0").
  explicit SoemFieldbusDriver(std::string ifname);

  /// @brief Closes the fieldbus if still open.
  ~SoemFieldbusDriver() override;

  SoemFieldbusDriver(const SoemFieldbusDriver&) = delete;
  SoemFieldbusDriver& operator=(const SoemFieldbusDriver&) = delete;

  /// @brief Opens the NIC, discovers slaves, maps PDOs, and transitions the
  ///        network to OP state.
  ///
  /// Safe to call once.  Returns an error if the interface cannot be opened,
  /// no slaves are found, or any slave fails to reach OP.
  std::expected<void, std::string> init() override;

  /// @brief Sends output PDOs and receives input PDOs in one LRW frame.
  ///
  /// Called once per GameLoop cycle.  Must not be called before a successful
  /// init() or after stop().
  void exchangeProcessData() override;

  /// @brief Transitions all slaves to INIT state and closes the NIC.
  void stop() override;

  /// @brief Number of slaves found during init().  Zero before init().
  int slaveCount() const;

 private:
  std::string ifname_;
  // ecx_contextt holds EC_MAXSLAVE (200) slave entries — too large for the
  // stack, so we heap-allocate it.
  std::unique_ptr<ecx_context> ctx_;
  static constexpr int kIomapSize = 4096;
  std::array<uint8_t, kIomapSize> iomap_{};
  int slave_count_{0};
};

}  // namespace mm::comm::soem
