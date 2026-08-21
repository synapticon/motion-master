#include "node/monitoring_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <iterator>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "core/util.h"
#include "node/device_parameter.h"
#include "node/process_image.h"

namespace mm::node {

namespace {

/// Serialises one sampled value to JSON: @c null when absent, otherwise the variant alternative
/// (integers/floats → number, string → string, raw bytes → array of numbers).
nlohmann::json valueToJson(const std::optional<DeviceParameterValue>& value) {
  if (!value) {
    return nlohmann::json();  // null
  }
  return std::visit([](const auto& v) { return nlohmann::json(v); }, *value);
}

}  // namespace

MonitoringManager::MonitoringManager(DeviceManager& deviceManager)
    : deviceManager_(deviceManager), refresher_(deviceManager) {}

MonitoringManager::~MonitoringManager() { stop(); }

void MonitoringManager::setPublish(PublishFn publish) {
  std::lock_guard<std::mutex> lock(mutex_);
  publish_ = std::move(publish);
}

std::expected<Monitoring, std::string> MonitoringManager::create(Monitoring config) {
  if (!mm::core::isUrlSafeId(config.topic)) {
    return std::unexpected("invalid topic: must match [A-Za-z0-9._-]{1,64}");
  }
  // interval is the flush cadence, not a sample rate. Bounded so each batch stays a sane size: a
  // longer interval ships more recorded cycles per message (~one row per cycle), so 2000 ms caps
  // the burst while 5 ms avoids a message storm without dropping any cycle.
  if (config.interval < std::chrono::milliseconds(5) ||
      config.interval > std::chrono::milliseconds(2000)) {
    return std::unexpected("interval must be between 5 ms and 2000 ms");
  }
  if (config.parameters.empty()) {
    return std::unexpected("parameters must not be empty");
  }

  // Classify every parameter, and if one cannot be sourced yet, auto-enumerate its device's object
  // dictionary once and retry: a PDO-mapped object needs its CoE data type to decode the image
  // bytes, and an SDO object needs its dictionary entry — neither is known until the OD is read.
  // This does the "read parameters" step implicitly, so monitoring a freshly-OP device just works.
  // Done before taking the monitoring lock because enumeration issues SDO and can take seconds; it
  // touches only the device's own (separately locked) parameter cache and never half-registers a
  // monitoring on failure.
  auto classify = [&](const MonitoredParameter& p) -> std::optional<ParamPlan> {
    if (auto spec = deviceManager_.pdoSampleSpec(p.devicePosition, p.index, p.subindex)) {
      return ParamPlan{p.devicePosition, p.index, p.subindex, Source::Pdo, spec};
    }
    const auto device = deviceManager_.deviceAt(p.devicePosition);
    if (device && device->parameterValue(p.index, p.subindex).has_value()) {
      return ParamPlan{p.devicePosition, p.index, p.subindex, Source::Sdo, std::nullopt};
    }
    return std::nullopt;
  };

  std::vector<ParamPlan> plans;
  plans.reserve(config.parameters.size());
  for (const auto& p : config.parameters) {
    auto plan = classify(p);
    if (!plan) {
      // Not sourceable yet. If this device's object dictionary is not enumerated, read it
      // once (the "read parameters" step, done implicitly) and retry — a PDO object then resolves
      // its data type and an SDO object its entry. A device that is already enumerated has a
      // non-empty map and is skipped, so a genuinely-absent object errors immediately instead of
      // triggering a wasteful re-read.
      const auto device = deviceManager_.deviceAt(p.devicePosition);
      if (device && !device->hasParameters()) {
        if (auto r = deviceManager_.initializeDeviceParameters(p.devicePosition, false); !r) {
          spdlog::debug("monitoring '{}': object-dictionary read of device {} failed: {}",
                        config.topic, p.devicePosition, r.error());
        }
        plan = classify(p);
      }
    }
    if (!plan) {
      return std::unexpected(std::format(
          "device {} object 0x{:04X}:{:02X} is neither PDO-mapped nor in the object dictionary",
          p.devicePosition, p.index, p.subindex));
    }
    plans.push_back(*plan);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (entries_.contains(config.topic)) {
    return std::unexpected("monitoring '" + config.topic + "' already exists");
  }

  // Register SDO parameters with the refresher (refcounted; poll period = the monitoring interval).
  for (const auto& plan : plans) {
    if (plan.source == Source::Sdo) {
      refresher_.acquire(plan.devicePosition, plan.index, plan.subindex, config.interval);
    }
  }

  Entry entry;
  entry.config = config;
  entry.plans = std::move(plans);
  entry.epoch = nextEpoch_++;
  entry.imageGeneration = deviceManager_.processImageGeneration();
  entries_.emplace(config.topic, std::move(entry));
  cv_.notify_one();  // wake the sampler thread to pick up the new monitoring (due immediately)
  return config;
}

bool MonitoringManager::remove(const std::string& topic) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(topic);
  if (it == entries_.end()) {
    return false;
  }
  for (const auto& plan : it->second.plans) {
    if (plan.source == Source::Sdo) {
      refresher_.release(plan.devicePosition, plan.index, plan.subindex);
    }
  }
  entries_.erase(it);
  cv_.notify_one();  // wake the sampler thread to recompute the nearest deadline
  return true;
}

std::optional<nlohmann::json> MonitoringManager::get(const std::string& topic) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(topic);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  return resourceJson(it->second);
}

nlohmann::json MonitoringManager::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto array = nlohmann::json::array();
  for (const auto& [topic, entry] : entries_) {
    array.push_back(resourceJson(entry));
  }
  return array;
}

void MonitoringManager::start() {
  refresher_.start();
  // Assigned under the lock — see ParameterRefresher::start for the start/stop window this closes.
  const std::lock_guard<std::mutex> lock(mutex_);
  if (running_) {
    return;
  }
  running_ = true;
  thread_ = std::thread([this] { run(); });
}

void MonitoringManager::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  refresher_.stop();
}

void MonitoringManager::run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (running_) {
    if (entries_.empty()) {
      cv_.wait(lock);  // nothing to sample — sleep until a create (or stop) wakes us
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    auto nearest = std::chrono::steady_clock::time_point::max();
    for (const auto& [topic, entry] : entries_) {
      nearest = std::min(nearest, entry.nextDue);
    }
    if (nearest <= now) {
      flushDue(lock, now, /*forceAll=*/false);
    } else {
      cv_.wait_until(lock, nearest);  // wake at the deadline, or earlier on create/remove/stop
    }
  }
}

void MonitoringManager::keepFresh(uint16_t devicePosition, uint16_t index, uint8_t subindex,
                                  std::chrono::milliseconds period) {
  // A pass-through and nothing more: the refresher already reference-counts, clamps the period and
  // polls through DeviceManager::readDeviceParameter, which stores into the parameter's cell — the
  // very cell a cyclic task reads. Owning the refresher is what makes this class the door.
  refresher_.acquire(devicePosition, index, subindex, period);
}

void MonitoringManager::stopKeepingFresh(uint16_t devicePosition, uint16_t index,
                                         uint8_t subindex) {
  refresher_.release(devicePosition, index, subindex);
}

void MonitoringManager::sampleAll() {
  std::unique_lock<std::mutex> lock(mutex_);
  flushDue(lock, std::chrono::steady_clock::now(), /*forceAll=*/true);
}

void MonitoringManager::flushDue(std::unique_lock<std::mutex>& lock,
                                 std::chrono::steady_clock::time_point now, bool forceAll) {
  auto states = takeDue(now, forceAll);
  if (states.empty()) {
    return;
  }
  // Copy the callback out too: publish_ is guarded by mutex_ and we are about to drop it.
  const PublishFn publish = publish_;
  lock.unlock();
  for (auto& state : states) {
    flushDetached(state, publish);
  }
  lock.lock();
  for (const auto& state : states) {
    commitFlush(state);
  }
}

std::vector<MonitoringManager::FlushState> MonitoringManager::takeDue(
    std::chrono::steady_clock::time_point now, bool forceAll) {
  std::vector<FlushState> states;
  for (auto& [topic, entry] : entries_) {
    if (!forceAll && entry.nextDue > now) {
      continue;
    }
    states.push_back(FlushState{.topic = topic,
                                .epoch = entry.epoch,
                                .interval = entry.config.interval,
                                .plans = entry.plans,
                                .cursor = entry.cursor,
                                .cursorPrimed = entry.cursorPrimed,
                                .imageGeneration = entry.imageGeneration});
    entry.nextDue = now + entry.config.interval;  // reschedule from now (no catch-up burst)
  }
  return states;
}

void MonitoringManager::commitFlush(const FlushState& state) {
  auto it = entries_.find(state.topic);
  // The epoch check is what makes releasing the lock safe: a remove + re-create of the same topic
  // while the flush was running produces a new registration with its own cursor, and writing this
  // flush's cursor onto it would skip the new monitoring past cycles it never delivered.
  if (it == entries_.end() || it->second.epoch != state.epoch) {
    return;
  }
  it->second.plans = state.plans;
  it->second.cursor = state.cursor;
  it->second.cursorPrimed = state.cursorPrimed;
  it->second.imageGeneration = state.imageGeneration;
}

std::size_t MonitoringManager::monitoringCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

std::size_t MonitoringManager::polledSdoCount() const { return refresher_.trackedCount(); }

void MonitoringManager::recaptureIfRemapped(FlushState& state) {
  const uint64_t generation = deviceManager_.processImageGeneration();
  if (generation == state.imageGeneration) {
    return;
  }
  // A re-map can change not just a mapped object's offset but whether it is PDO-mapped at all:
  // adding an object to the mapping flips a parameter SDO→PDO, removing one flips it PDO→SDO. The
  // generation only ever bumps on a successful (re)map that publishes a fresh image, so the freshly
  // published image is authoritative here — re-classify every plan against it and move it between
  // the PDO path and the SDO refresher accordingly, so both the sampled source and the reported
  // source stay correct after a remap (a stale classification would sample the wrong path — null
  // for a PDO plan whose object left, or a stale SDO cache for one that joined).
  for (auto& plan : state.plans) {
    auto spec = deviceManager_.pdoSampleSpec(plan.devicePosition, plan.index, plan.subindex);
    const Source newSource = spec ? Source::Pdo : Source::Sdo;
    if (newSource == plan.source) {
      plan.pdoSpec = spec;  // unchanged classification; offsets may have shifted
      continue;
    }
    if (newSource == Source::Sdo) {
      // PDO→SDO: the object left the image; start polling it in the background.
      refresher_.acquire(plan.devicePosition, plan.index, plan.subindex, state.interval);
      plan.pdoSpec = std::nullopt;
    } else {
      // SDO→PDO: the object joined the image; stop the now-redundant background poll.
      refresher_.release(plan.devicePosition, plan.index, plan.subindex);
      plan.pdoSpec = spec;
    }
    plan.source = newSource;
  }
  state.imageGeneration = generation;
}

void MonitoringManager::flushDetached(FlushState& state, const PublishFn& publish) {
  recaptureIfRemapped(state);

  const uint64_t head = deviceManager_.recorderHead();
  // Seed the cursor on the first flush so the monitoring streams from "now" rather than dumping the
  // whole ring history that predates it.
  if (!state.cursorPrimed) {
    state.cursor = head;
    state.cursorPrimed = true;
    return;
  }
  if (head == state.cursor) {
    return;  // no new cycles recorded since the last flush (bus idle / not exchanging)
  }
  // Resync a cursor that fell outside the live recorded span [oldest, head). Two causes:
  //   - it fell more than a whole ring behind (cursor < oldest): those cycles were overwritten
  //     before we read them.
  //   - a layout-changing re-map re-allocated the recorder ring, resetting head to a fresh, smaller
  //     sequence (cursor > head): the cursor now indexes a ring that no longer exists, and the
  //     unsigned head - cursor below would underflow into a gigantic rows.reserve() (length_error).
  // Either way, log the gap (never silent) and skip forward to the oldest record still present.
  const uint64_t oldest = deviceManager_.recorderOldestSeq();
  if (state.cursor < oldest || state.cursor > head) {
    spdlog::warn("monitoring '{}' resynced — cursor {} outside recorded span [{}, {})", state.topic,
                 state.cursor, oldest, head);
    state.cursor = oldest;
  }

  // Per-flush constants, evaluated once and applied to every row: the device live gate (current AL
  // state) and SDO values (the refresher cache is a single current value, so every row in this
  // flush shares it — SDO objects are slow telemetry, not per-cycle signals).
  std::unordered_map<uint16_t, bool> exchanging;
  auto isExchanging = [&](uint16_t pos) {
    auto [it, inserted] = exchanging.try_emplace(pos, false);
    if (inserted) {
      it->second = deviceManager_.deviceExchangesProcessData(pos);
    }
    return it->second;
  };
  std::vector<std::optional<DeviceParameterValue>> sdoValues(state.plans.size());
  for (size_t i = 0; i < state.plans.size(); ++i) {
    const auto& plan = state.plans[i];
    if (plan.source == Source::Sdo && isExchanging(plan.devicePosition)) {
      if (const auto device = deviceManager_.deviceAt(plan.devicePosition)) {
        sdoValues[i] = device->parameterValue(plan.index, plan.subindex);
      }
    }
  }

  // Decode every recorded cycle in [cursor, head) into one row each — the lossless span.
  std::vector<Sample> rows;
  rows.reserve(static_cast<size_t>(head - state.cursor));
  ProcessDataRing::Record record;
  for (uint64_t seq = state.cursor; seq != head; ++seq) {
    if (!deviceManager_.readRecord(seq, record)) {
      continue;  // raced an overwrite at the oldest edge — skip this one cycle
    }
    Sample sample;
    sample.timestampUs = static_cast<int64_t>(record.timestampNs / 1000);
    sample.values.reserve(state.plans.size());
    for (size_t i = 0; i < state.plans.size(); ++i) {
      const auto& plan = state.plans[i];
      std::optional<DeviceParameterValue> value;  // null unless resolved below
      if (isExchanging(plan.devicePosition)) {
        if (plan.source == Source::Pdo) {
          if (plan.pdoSpec) {
            const std::vector<uint8_t>& region =
                plan.pdoSpec->isOutput ? record.outputs : record.inputs;
            auto bytes = extractBits(std::span<const uint8_t>(region.data(), region.size()),
                                     plan.pdoSpec->bitOffset, plan.pdoSpec->bitLength);
            if (auto decoded = decodeSdoBytes(plan.pdoSpec->dataType, bytes)) {
              value = std::move(*decoded);
            }
          }
        } else {
          value = sdoValues[i];
        }
      }
      sample.values.push_back(std::move(value));
    }
    rows.push_back(std::move(sample));
  }
  state.cursor = head;

  // The caller's copy, not publish_ — that member is guarded by mutex_, which is deliberately not
  // held here.
  if (!publish || rows.empty()) {
    return;
  }
  auto data = nlohmann::json::array();
  for (const auto& sample : rows) {
    auto row = nlohmann::json::array();
    row.push_back(sample.timestampUs);
    std::transform(sample.values.begin(), sample.values.end(), std::back_inserter(row),
                   valueToJson);
    data.push_back(std::move(row));
  }
  const nlohmann::json envelope = {
      {"type", "monitoring"}, {"topic", state.topic}, {"data", std::move(data)}};
  // `replace` handler: a string-typed param carrying non-UTF-8 bytes must not throw here — this
  // runs on the sampler thread with no surrounding catch, so a throw would terminate the server.
  publish(state.topic, envelope.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

nlohmann::json MonitoringManager::resourceJson(const Entry& entry) {
  nlohmann::json j = entry.config;  // {topic, name?, interval, parameters:[...]}
  // Replace the bare parameter list with one annotated by how each value is sourced (for the UI).
  auto parameters = nlohmann::json::array();
  std::transform(entry.plans.begin(), entry.plans.end(), std::back_inserter(parameters),
                 [](const auto& plan) {
                   return nlohmann::json{{"devicePosition", plan.devicePosition},
                                         {"index", plan.index},
                                         {"subindex", plan.subindex},
                                         {"source", plan.source == Source::Pdo ? "pdo" : "sdo"}};
                 });
  j["parameters"] = std::move(parameters);
  return j;
}

}  // namespace mm::node
