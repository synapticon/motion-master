#pragma once

#include <cstdint>
#include <memory>
#include <string>

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

  /// @brief Sends output PDOs and receives input PDOs in one LRW frame.
  ///
  /// Called once per @c GameLoop cycle.  Must not be called before a successful
  /// @c init() or after @c stop().
  void exchangeProcessData() override;

  /// @brief Closes the NIC and releases all driver resources.
  void stop() override;

  /// @brief Returns the number of discovered slaves, or 0 before discovery.
  int slaveCount() const;

 private:
  std::string ifname_;
  // ecx_contextt is several hundred KB (EC_MAXSLAVE slave entries) — heap-
  // allocated and null until init() succeeds.
  std::unique_ptr<ecx_context> ctx_;
  uint8_t map_[4096]{};
};

}  // namespace mm::comm::soem
