#pragma once

#include <expected>
#include <string>

namespace mm::comm {

/// @brief Abstract interface for a fieldbus driver.
///
/// Concrete implementations are SoemDriver (SOEM), SpoeDriver (SPoE), and
/// IghDriver (IgH EtherCAT).  App instantiates exactly one and injects it into
/// DeviceManager and GameLoop via dependency injection.
class IFieldbusDriver {
 public:
  /// @brief Virtual destructor.
  virtual ~IFieldbusDriver() = default;

  /// @brief Initialises the fieldbus hardware and prepares for cyclic operation.
  ///
  /// Must be called once before exchangeProcessData().  Discovers slaves, maps
  /// PDOs, and transitions the network to OP state.
  ///
  /// @return Void on success, or an error message on failure.
  virtual std::expected<void, std::string> init() = 0;

  /// @brief Exchanges process data with all slaves in one EtherCAT frame.
  ///
  /// Called once per GameLoop cycle.  Must complete within the cycle budget.
  /// Timing jitter here propagates directly to control latency.
  virtual void exchangeProcessData() = 0;

  /// @brief Releases fieldbus hardware resources and closes the network.
  ///
  /// After stop() returns, exchangeProcessData() must not be called again.
  virtual void stop() = 0;
};

}  // namespace mm::comm
