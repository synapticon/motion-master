#include "node/monitoring_manager.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "core/util.h"
#include "node/device_parameter.h"
#include "node/process_image.h"

namespace mm::node {

namespace {

constexpr char kReservedTopic[] = "pdos";  // the built-in high-frequency PDO stream's topic

/// Wall-clock timestamp in epoch milliseconds (fits a JS number; used as each row's time key).
int64_t nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

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
  if (config.topic == kReservedTopic) {
    return std::unexpected("topic 'pdos' is reserved");
  }
  if (config.interval < std::chrono::milliseconds{1}) {
    return std::unexpected("interval must be >= 1 ms");
  }
  if (config.bufferSize < 16) {
    return std::unexpected("bufferSize must be >= 16");
  }
  if (config.parameters.empty()) {
    return std::unexpected("parameters must not be empty");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (entries_.contains(config.topic)) {
    return std::unexpected("monitoring '" + config.topic + "' already exists");
  }

  // Classify every parameter first and reject the whole request on the first that cannot be
  // sourced, so a failed create never half-registers a monitoring or leaks a refresher reference.
  std::vector<ParamPlan> plans;
  plans.reserve(config.parameters.size());
  for (const auto& p : config.parameters) {
    ParamPlan plan{p.devicePosition, p.index, p.subindex, Source::Sdo, std::nullopt};
    if (auto spec = deviceManager_.pdoSampleSpec(p.devicePosition, p.index, p.subindex)) {
      plan.source = Source::Pdo;
      plan.pdoSpec = spec;
    } else if (deviceManager_.value(p.devicePosition, p.index, p.subindex).has_value()) {
      plan.source = Source::Sdo;
    } else {
      return std::unexpected(std::format(
          "device {} object 0x{:04X}:{:02X} is neither PDO-mapped nor in the object dictionary",
          p.devicePosition, p.index, p.subindex));
    }
    plans.push_back(plan);
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

void MonitoringManager::start() { refresher_.start(); }

void MonitoringManager::stop() { refresher_.stop(); }

void MonitoringManager::sampleAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [topic, entry] : entries_) {
    sampleEntry(entry);
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
  // A re-map may have shifted offsets — re-capture each PDO parameter's spec (nullopt if the
  // object is no longer mapped, in which case it samples null until it returns).
  for (auto& plan : entry.plans) {
    if (plan.source == Source::Pdo) {
      plan.pdoSpec = deviceManager_.pdoSampleSpec(plan.devicePosition, plan.index, plan.subindex);
    }
  }
  entry.imageGeneration = generation;
}

void MonitoringManager::sampleEntry(Entry& entry) {
  recaptureIfRemapped(entry);

  // Read each direction's image once, so every value in the row comes from the same bus cycle.
  const ProcessBuffer inputs = deviceManager_.inputSnapshot();
  const ProcessBuffer outputs = deviceManager_.outputSnapshot();

  // Cache the per-device live gate for this row (a row may span several devices).
  std::unordered_map<uint16_t, bool> exchanging;
  auto isExchanging = [&](uint16_t pos) {
    auto [it, inserted] = exchanging.try_emplace(pos, false);
    if (inserted) {
      it->second = deviceManager_.deviceExchangesProcessData(pos);
    }
    return it->second;
  };

  Sample sample;
  sample.timestampMs = nowMillis();
  sample.values.reserve(entry.plans.size());
  for (const auto& plan : entry.plans) {
    std::optional<DeviceParameterValue> value;  // null unless resolved live below
    if (isExchanging(plan.devicePosition)) {
      if (plan.source == Source::Pdo) {
        if (plan.pdoSpec) {
          const ProcessBuffer& buffer = plan.pdoSpec->isOutput ? outputs : inputs;
          auto bytes = extractBits(std::span<const uint8_t>(buffer.bytes.data(), buffer.size),
                                   plan.pdoSpec->bitOffset, plan.pdoSpec->bitLength);
          if (auto decoded = decodeSdoBytes(plan.pdoSpec->dataType, bytes)) {
            value = std::move(*decoded);
          }
        }
      } else {
        value = deviceManager_.value(plan.devicePosition, plan.index, plan.subindex);
      }
    }
    sample.values.push_back(std::move(value));
  }

  entry.batch.push_back(std::move(sample));
  if (entry.batch.size() >= entry.config.bufferSize) {
    flush(entry);
  }
}

void MonitoringManager::flush(Entry& entry) {
  if (publish_) {
    auto data = nlohmann::json::array();
    for (const auto& sample : entry.batch) {
      auto row = nlohmann::json::array();
      row.push_back(sample.timestampMs);
      for (const auto& value : sample.values) {
        row.push_back(valueToJson(value));
      }
      data.push_back(std::move(row));
    }
    const nlohmann::json envelope = {
        {"type", "monitoring"}, {"topic", entry.config.topic}, {"data", std::move(data)}};
    publish_(entry.config.topic, envelope.dump());
  }
  entry.batch.clear();
}

nlohmann::json MonitoringManager::resourceJson(const Entry& entry) const {
  nlohmann::json j = entry.config;  // {topic, name?, interval, bufferSize, parameters:[...]}
  // Replace the bare parameter list with one annotated by how each value is sourced (for the UI).
  auto parameters = nlohmann::json::array();
  for (const auto& plan : entry.plans) {
    parameters.push_back({{"devicePosition", plan.devicePosition},
                          {"index", plan.index},
                          {"subindex", plan.subindex},
                          {"source", plan.source == Source::Pdo ? "pdo" : "sdo"}});
  }
  j["parameters"] = std::move(parameters);
  j["bufferFill"] = entry.batch.size();
  return j;
}

}  // namespace mm::node
