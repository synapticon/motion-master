#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "node/device_manager.h"
#include "node/monitoring.h"
#include "node/parameter_refresher.h"

namespace mm::node {

/// @brief Owns the active monitorings and turns each into a stream of sampled rows.
///
/// A client creates a @c Monitoring (topic, interval, bufferSize, parameters); the manager
/// validates it, classifies every parameter by how its value is sourced, samples them off the RT
/// thread, batches @c bufferSize rows, and hands each batch to an injected publish callback.
///
/// Sourcing (decided once at @c create, against the published process image):
/// - **PDO** — the object is mapped in the live image; its value is decoded each tick straight
///   from one coherent @c inputSnapshot / @c outputSnapshot (no bus access, all values in a row
///   from the same cycle). The decode spec is captured up front and re-captured on a re-map.
/// - **SDO** — the object is not PDO-mapped; it is registered with the owned @c ParameterRefresher
///   which polls it in the background, and the sampler reads the cached value.
///
/// Monitoring is live-only: a parameter whose owning device is not exchanging (SAFE-OP/OP) samples
/// @c null. Topics are unique; the WebSocket pub/sub topic @c "pdos" is reserved.
///
/// Thread-safe. Owns a private @c ParameterRefresher (the sampler is its sole client). The App
/// wires only this manager (with a @c DeviceManager& and a publish callback).
class MonitoringManager {
 public:
  /// @brief Publishes one batch: @p json (a @c {"type":"monitoring","topic",...} envelope) under
  ///        @p topic. Wired in the composition root to the WebSocket server's topic publish.
  using PublishFn = std::function<void(std::string topic, std::string json)>;

  /// @brief Constructs the manager over @p deviceManager. Does not start sampling — call start().
  explicit MonitoringManager(DeviceManager& deviceManager);

  /// @brief Stops the owned refresher (and, once added, the sampler thread).
  ~MonitoringManager();

  MonitoringManager(const MonitoringManager&) = delete;
  MonitoringManager& operator=(const MonitoringManager&) = delete;

  /// @brief Sets the batch publish callback. Call before @c start().
  void setPublish(PublishFn publish);

  /// @brief Validates @p config, classifies its parameters, registers SDO ones with the refresher,
  ///        and registers the monitoring.
  ///
  /// Validation: @c topic URL-safe and not the reserved @c "pdos"; not already registered;
  /// @c interval >= 1 ms; @c bufferSize >= 16; @c parameters non-empty; and every parameter is
  /// either PDO-mapped or present in its device's object dictionary (otherwise it cannot be
  /// sourced).
  ///
  /// @return The created configuration on success, or an error string describing the first
  ///         validation failure.
  std::expected<Monitoring, std::string> create(Monitoring config);

  /// @brief Removes a monitoring, releasing its SDO parameters from the refresher.
  /// @return @c true if a monitoring with @p topic existed, @c false otherwise.
  bool remove(const std::string& topic);

  /// @brief Returns the monitoring resource as JSON (config + per-parameter @c source + buffer
  ///        fill), or @c nullopt if @p topic is unknown.
  std::optional<nlohmann::json> get(const std::string& topic) const;

  /// @brief Returns all monitoring resources as a JSON array, in topic order.
  nlohmann::json list() const;

  /// @brief Starts the owned refresher (and, once added, the sampler thread). Idempotent.
  void start();

  /// @brief Stops the owned refresher (and, once added, the sampler thread). Idempotent.
  void stop();

  /// @brief Samples every monitoring once, appending a row and flushing a batch when full.
  ///        The deterministic core the (later) scheduler thread drives; exposed for tests.
  void sampleAll();

  /// @brief Number of registered monitorings. For tests / status.
  std::size_t monitoringCount() const;

  /// @brief Number of distinct SDO objects currently polled by the refresher. For tests / status.
  std::size_t polledSdoCount() const;

 private:
  /// @brief How a monitored parameter's value is obtained.
  enum class Source { Pdo, Sdo };

  /// @brief A parameter resolved to its source: PDO carries the captured decode spec (refreshed
  ///        on a re-map), SDO is served from the refresher-fed cache.
  struct ParamPlan {
    uint16_t devicePosition;
    uint16_t index;
    uint8_t subindex;
    Source source;
    std::optional<DeviceManager::PdoSampleSpec> pdoSpec;  // PDO only
  };

  /// @brief One sampled row: a timestamp and one optional value per parameter (@c nullopt = null).
  struct Sample {
    int64_t timestampMs;
    std::vector<std::optional<DeviceParameterValue>> values;
  };

  /// @brief A registered monitoring and its runtime state.
  struct Entry {
    Monitoring config;
    std::vector<ParamPlan> plans;
    std::vector<Sample> batch;
    uint64_t imageGeneration = 0;  // processImageGeneration the PDO specs were captured under
    std::chrono::steady_clock::time_point nextDue{};  // default (epoch) => due on the next wake
  };

  void sampleEntry(Entry& entry);          // assumes mutex_ held
  void recaptureIfRemapped(Entry& entry);  // re-capture PDO specs; assumes mutex_ held
  void flush(Entry& entry);                // publish batch + clear; assumes mutex_ held
  static nlohmann::json resourceJson(const Entry& entry);  // assumes mutex_ held

  /// @brief Sampler thread body: waits until the nearest monitoring is due (or an
  ///        acquire/remove/stop wakes it), then samples every due monitoring and reschedules it.
  void run();

  DeviceManager& deviceManager_;
  ParameterRefresher refresher_;
  mutable std::mutex mutex_;    ///< Guards entries_ and running_.
  std::condition_variable cv_;  ///< Wakes the sampler thread on create/remove/stop.
  std::map<std::string, Entry> entries_;
  PublishFn publish_;
  bool running_ = false;  ///< Whether the sampler thread should keep looping.
  std::thread thread_;
};

}  // namespace mm::node
