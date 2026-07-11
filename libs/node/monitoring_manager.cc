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
    if (deviceManager_.value(p.devicePosition, p.index, p.subindex).has_value()) {
      return ParamPlan{p.devicePosition, p.index, p.subindex, Source::Sdo, std::nullopt};
    }
    return std::nullopt;
  };

  std::vector<ParamPlan> plans;
  plans.reserve(config.parameters.size());
  for (const auto& p : config.parameters) {
    auto plan = classify(p);
    if (!plan) {
      // Not sourceable yet. If this device's object dictionary has not been enumerated, read it
      // once (the "read parameters" step, done implicitly) and retry — a PDO object then resolves
      // its data type and an SDO object its entry. A device that is already enumerated has a
      // non-empty map and is skipped, so a genuinely-absent object errors immediately instead of
      // triggering a wasteful re-read.
      const Device* device = deviceManager_.findDevice(p.devicePosition);
      if (device && device->parameters().empty()) {
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
    plans.push_back(std::move(*plan));
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
  std::unique_lock<std::mutex> lock(mutex_);
  if (running_) {
    return;
  }
  running_ = true;
  lock.unlock();
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
      for (auto& [topic, entry] : entries_) {
        if (entry.nextDue <= now) {
          flushEntry(entry);
          entry.nextDue = now + entry.config.interval;  // reschedule from now (no catch-up burst)
        }
      }
    } else {
      cv_.wait_until(lock, nearest);  // wake at the deadline, or earlier on create/remove/stop
    }
  }
}

void MonitoringManager::sampleAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [topic, entry] : entries_) {
    flushEntry(entry);
  }
}

std::size_t MonitoringManager::monitoringCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

std::size_t MonitoringManager::polledSdoCount() const { return refresher_.trackedCount(); }

void MonitoringManager::recaptureIfRemapped(Entry& entry) {
  const uint64_t generation = deviceManager_.processImageGeneration();
  if (generation == entry.imageGeneration) {
    return;
  }
  // A re-map can change not just a mapped object's offset but whether it is PDO-mapped at all:
  // adding an object to the mapping flips a parameter SDO→PDO, removing one flips it PDO→SDO. The
  // generation only ever bumps on a successful (re)map that publishes a fresh image, so the freshly
  // published image is authoritative here — re-classify every plan against it and move it between
  // the PDO path and the SDO refresher accordingly, so both the sampled source and the reported
  // source stay correct after a remap (a stale classification would sample the wrong path — null
  // for a PDO plan whose object left, or a stale SDO cache for one that joined).
  for (auto& plan : entry.plans) {
    auto spec = deviceManager_.pdoSampleSpec(plan.devicePosition, plan.index, plan.subindex);
    const Source newSource = spec ? Source::Pdo : Source::Sdo;
    if (newSource == plan.source) {
      plan.pdoSpec = std::move(spec);  // unchanged classification; offsets may have shifted
      continue;
    }
    if (newSource == Source::Sdo) {
      // PDO→SDO: the object left the image; start polling it in the background.
      refresher_.acquire(plan.devicePosition, plan.index, plan.subindex, entry.config.interval);
      plan.pdoSpec = std::nullopt;
    } else {
      // SDO→PDO: the object joined the image; stop the now-redundant background poll.
      refresher_.release(plan.devicePosition, plan.index, plan.subindex);
      plan.pdoSpec = std::move(spec);
    }
    plan.source = newSource;
  }
  entry.imageGeneration = generation;
}

void MonitoringManager::flushEntry(Entry& entry) {
  recaptureIfRemapped(entry);

  const uint64_t head = deviceManager_.recorderHead();
  // Seed the cursor on the first flush so the monitoring streams from "now" rather than dumping the
  // whole ring history that predates it.
  if (!entry.cursorPrimed) {
    entry.cursor = head;
    entry.cursorPrimed = true;
    return;
  }
  if (head == entry.cursor) {
    return;  // no new cycles recorded since the last flush (bus idle / not exchanging)
  }
  // Resync a cursor that has fallen outside the live recorded span [oldest, head). Two causes:
  //   - it fell more than a whole ring behind (cursor < oldest): those cycles were overwritten
  //     before we read them.
  //   - a layout-changing re-map re-allocated the recorder ring, resetting head to a fresh, smaller
  //     sequence (cursor > head): the cursor now indexes a ring that no longer exists, and the
  //     unsigned head - cursor below would underflow into a gigantic rows.reserve() (length_error).
  // Either way, log the gap (never silent) and skip forward to the oldest record still present.
  const uint64_t oldest = deviceManager_.recorderOldestSeq();
  if (entry.cursor < oldest || entry.cursor > head) {
    spdlog::warn("monitoring '{}' resynced — cursor {} outside recorded span [{}, {})",
                 entry.config.topic, entry.cursor, oldest, head);
    entry.cursor = oldest;
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
  std::vector<std::optional<DeviceParameterValue>> sdoValues(entry.plans.size());
  for (size_t i = 0; i < entry.plans.size(); ++i) {
    const auto& plan = entry.plans[i];
    if (plan.source == Source::Sdo && isExchanging(plan.devicePosition)) {
      sdoValues[i] = deviceManager_.value(plan.devicePosition, plan.index, plan.subindex);
    }
  }

  // Decode every recorded cycle in [cursor, head) into one row each — the lossless span.
  std::vector<Sample> rows;
  rows.reserve(static_cast<size_t>(head - entry.cursor));
  ProcessDataRing::Record record;
  for (uint64_t seq = entry.cursor; seq != head; ++seq) {
    if (!deviceManager_.readRecord(seq, record)) {
      continue;  // raced an overwrite at the oldest edge — skip this one cycle
    }
    Sample sample;
    sample.timestampUs = static_cast<int64_t>(record.timestampNs / 1000);
    sample.values.reserve(entry.plans.size());
    for (size_t i = 0; i < entry.plans.size(); ++i) {
      const auto& plan = entry.plans[i];
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
  entry.cursor = head;

  if (!publish_ || rows.empty()) {
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
      {"type", "monitoring"}, {"topic", entry.config.topic}, {"data", std::move(data)}};
  // `replace` handler: a string-typed param carrying non-UTF-8 bytes must not throw here — this
  // runs on the sampler thread with no surrounding catch, so a throw would terminate the server.
  publish_(entry.config.topic,
           envelope.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
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
