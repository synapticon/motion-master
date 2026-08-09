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

/// @brief Owns the active monitorings and turns each into a lossless stream of recorded rows.
///
/// A client creates a @c Monitoring (topic, interval, parameters); the manager validates it,
/// classifies every parameter by how its value is sourced, and on each flush ships **every**
/// process-data cycle recorded since the last flush. The stream is lossless: each monitoring holds
/// a read cursor into the recorder ring and, each time it is due, decodes and publishes every
/// cycle-row in @c [cursor, recorderHead()) as one batch, then advances the cursor. @c interval is
/// the flush cadence, not a sample rate — a longer interval means a bigger batch, never dropped
/// cycles. A cursor that falls more than a whole ring behind (a stalled client) is logged and
/// resynced to the oldest available cycle; the gap is never silent.
///
/// Sourcing (decided once at @c create, against the published process image):
/// - **PDO** — the object is mapped in the live image; its value is decoded for each recorded
///   cycle from that cycle's ring record (no bus access, all values in a row from the same cycle).
///   The decode spec is captured up front and re-captured on a re-map.
/// - **SDO** — the object is not PDO-mapped; it is registered with the owned @c ParameterRefresher
///   which polls it in the background, and the sampler reads the cached value. See *Why a refresher
///   exists at all* below.
///
/// Monitoring is live-only: a parameter whose owning device is not exchanging (SAFE-OP/OP) samples
/// @c null. Topics are unique.
///
/// **Why a refresher exists at all.** The sampler must never touch the bus: it serves a lossless
/// per-cycle stream, and an SDO upload is a blocking mailbox round-trip that queues behind the
/// driver's control-plane lock — one per cycle per object is not an option at kHz rates. PDO
/// parameters need no bus access (they decode out of the recorder ring), but an SDO-sourced
/// parameter has no cyclic source at all, so something has to fetch it on a slower cadence, off the
/// flush path. The refresher polls into each device's parameter cache in the background and the
/// flush reads only that cache — which is why every row in one batch shares the same SDO value
/// (slow telemetry, not a per-cycle signal). This manager drives @c acquire / @c release rather
/// than merely reading, because the classification is dynamic: a re-map can flip a parameter
/// PDO→SDO or SDO→PDO, and @c recaptureIfRemapped moves it between the two.
///
/// Thread-safe. Owns a private @c ParameterRefresher because monitoring is its only client today.
/// That is an ownership choice, not a coupling: the refresher depends on nothing but
/// @c DeviceManager&, its entries are reference-counted per object, and its @c start / @c stop are
/// idempotent — so it is usable on its own, and a second client would be served by constructing it
/// in the composition root and injecting it here instead. The App wires only this manager (with a
/// @c DeviceManager& and a publish callback).
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
  /// Validation: @c topic URL-safe; not already registered;
  /// @c interval between 5 ms and 2000 ms (the flush cadence); @c parameters non-empty; and every
  /// parameter is either PDO-mapped or present in its device's object dictionary (otherwise it
  /// cannot be sourced).
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

  /// @brief Flushes every monitoring once: delivers each one's recorded cycles since its last
  ///        flush. The deterministic core the scheduler thread drives; exposed for tests.
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
    uint16_t devicePosition = 0;
    uint16_t index = 0;
    uint8_t subindex = 0;
    Source source = Source::Pdo;
    std::optional<DeviceManager::PdoSampleSpec> pdoSpec;  // PDO only
  };

  /// @brief One row: a cycle timestamp (epoch microseconds) and one optional value per parameter
  ///        (@c nullopt = null). Microseconds keep every cycle distinct even at sub-ms periods and
  ///        stay exact in a JavaScript double (see @c ProcessDataRing for the unit rationale).
  struct Sample {
    int64_t timestampUs = 0;
    std::vector<std::optional<DeviceParameterValue>> values;
  };

  /// @brief A registered monitoring and its runtime state.
  struct Entry {
    Monitoring config;
    std::vector<ParamPlan> plans;
    uint64_t epoch = 0;            // identifies this registration across a remove + re-create
    uint64_t cursor = 0;           // next recorder sequence number to deliver ([cursor, head))
    bool cursorPrimed = false;     // false until the first flush seeds cursor from recorderHead()
    uint64_t imageGeneration = 0;  // processImageGeneration the PDO specs were captured under
    std::chrono::steady_clock::time_point nextDue{};  // default (epoch) => due on the next wake
  };

  /// @brief One entry's flushable state, carried out of @c mutex_ and back in.
  ///
  /// A flush is the only thing this class does that calls into @c DeviceManager, and it is by far
  /// the longest — it walks every recorded cycle since the cursor. Doing it under @c mutex_ meant a
  /// control-plane operation that held the bus lock stalled not just the sampler but every
  /// @c /api/monitorings endpoint. So the flush works on a detached copy: snapshot under the lock
  /// (@c takeDue), run the DeviceManager reads and the publish with no lock held (@c
  /// flushDetached), then write the advanced cursor back (@c commitFlush). @c
  /// ParameterRefresher::pollDue does the same thing for the same reason.
  struct FlushState {
    std::string topic;
    uint64_t epoch = 0;
    std::chrono::milliseconds interval{};  // poll period handed to the refresher on an SDO switch
    std::vector<ParamPlan> plans;
    uint64_t cursor = 0;
    bool cursorPrimed = false;
    uint64_t imageGeneration = 0;
  };

  /// @brief Snapshots every entry due at @p now (all of them when @p forceAll) and reschedules it.
  ///        Assumes @c mutex_ held.
  std::vector<FlushState> takeDue(std::chrono::steady_clock::time_point now, bool forceAll);

  /// @brief Decodes [cursor, head) and publishes it, advancing @p state's cursor.
  ///        @c mutex_ must NOT be held.
  void flushDetached(FlushState& state, const PublishFn& publish);

  /// @brief Writes a completed flush back onto its entry, if that entry is still registered under
  ///        the same epoch. Assumes @c mutex_ held.
  void commitFlush(const FlushState& state);

  /// @brief Runs one round of due flushes, releasing @p lock across the DeviceManager reads.
  ///        Called with @p lock held; returns with it held.
  void flushDue(std::unique_lock<std::mutex>& lock, std::chrono::steady_clock::time_point now,
                bool forceAll);

  /// @brief Re-classifies each plan's source against the freshly published image and re-captures
  ///        its decode spec. @c mutex_ must NOT be held (it calls into @c DeviceManager).
  void recaptureIfRemapped(FlushState& state);

  static nlohmann::json resourceJson(const Entry& entry);  // assumes mutex_ held

  /// @brief Sampler thread body: waits until the nearest monitoring is due (or an
  ///        acquire/remove/stop wakes it), then samples every due monitoring and reschedules it.
  void run();

  DeviceManager& deviceManager_;
  ParameterRefresher refresher_;
  mutable std::mutex mutex_;    ///< Guards entries_, running_ and nextEpoch_.
  std::condition_variable cv_;  ///< Wakes the sampler thread on create/remove/stop.
  std::map<std::string, Entry> entries_;
  uint64_t nextEpoch_ = 1;  ///< Stamped onto each new Entry; see FlushState.
  PublishFn publish_;
  bool running_ = false;  ///< Whether the sampler thread should keep looping.
  std::thread thread_;
};

}  // namespace mm::node
