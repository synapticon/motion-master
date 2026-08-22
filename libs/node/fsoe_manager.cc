#include "node/fsoe_manager.h"

#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "node/device_manager.h"

namespace mm::node {
namespace {

nlohmann::json processValuesJson(const mm::etg::SdpProcessValues& values) {
  return nlohmann::json{
      {"position", values.positionFixedPoint},
      {"positionRevolutions", mm::etg::sdpPositionRevolutions(values.positionFixedPoint)},
      {"positionValid", values.positionValid},
      {"positionReferenced", values.positionReferenced},
      {"velocityMilliRpm", values.velocityMilliRpm},
      {"velocityValid", values.velocityValid},
      {"torqueMillinewtonMetres", values.torqueMillinewtonMetres},
      {"torqueValid", values.torqueValid},
      {"crossCheckOk", values.crossCheckOk},
  };
}

}  // namespace

void to_json(nlohmann::json& j, const FsoeConnectionReport& report) {
  const FsoeConnectionState& state = report.state;
  const mm::etg::SdpStatus status = state.safetyStatus();
  j = nlohmann::json{
      {"slavePosition", report.config.slavePosition},
      {"slaveAddress", report.config.slaveAddress},
      {"connectionId", report.config.connectionId},
      {"watchdogMs", report.config.watchdogMs},
      {"rxPdoIndex", report.config.rxPdoIndex},
      {"txPdoIndex", report.config.txPdoIndex},
      {"state", mm::etg::fsoeStateName(state.state)},
      {"bound", state.bound},
      {"inputsValid", state.inputsValid},
      {"dataCommand", mm::etg::fsoeCommandName(state.dataCommand)},
      {"fault", mm::etg::fsoeErrorName(state.fault)},
      {"peerFaultCode", state.peerFaultCode},
      {"sessionId", state.sessionId},
      {"cycles", state.cycles},
      {"framesAccepted", state.framesAccepted},
      {"faults", state.faults},
      {"safeInputs", std::vector<uint8_t>(state.safeInputs.begin(),
                                          state.safeInputs.begin() + state.safeInputsLength)},
      {"safeOutputs", std::vector<uint8_t>(state.safeOutputs.begin(),
                                           state.safeOutputs.begin() + state.safeOutputsLength)},
      {"safetyStatus", {{"stoActive", status.stoActive}, {"error", status.error}}},
      {"processValues", processValuesJson(state.processValues())},
  };
}

std::expected<FsoeConnectionReport, std::string> FsoeManager::open(
    const FsoeConnectionConfig& config) {
  const std::lock_guard<std::mutex> lock(mutex_);

  const size_t count = publishedCount_.load(std::memory_order_relaxed);
  if (count == kMaxConnections) {
    return std::unexpected(std::format(
        "no room for another connection: {} have been opened in this session, which is the limit",
        kMaxConnections));
  }

  auto connection = FsoeConnection::open(deviceManager_, config);
  if (!connection) {
    return std::unexpected(connection.error());
  }

  // Retire any connection to the same drive only once the new one is built: a failed open leaves
  // the drive exactly as it was, still driven.
  for (size_t i = 0; i < count; ++i) {
    if (published_[i]->config().slavePosition == config.slavePosition) {
      published_[i]->close();
    }
  }

  FsoeConnection* raw = connection->get();
  owned_.push_back(std::move(*connection));
  published_[count] = raw;
  publishedCount_.store(count + 1, std::memory_order_release);

  return FsoeConnectionReport{.config = raw->config(), .state = raw->state()};
}

std::expected<void, std::string> FsoeManager::close(uint16_t slavePosition) {
  const std::lock_guard<std::mutex> lock(mutex_);
  FsoeConnection* connection = find(slavePosition);
  if (connection == nullptr) {
    return std::unexpected(std::format("no FSoE connection is open to device {}", slavePosition));
  }
  connection->close();
  return {};
}

FsoeConnection* FsoeManager::find(uint16_t slavePosition) {
  const size_t count = publishedCount_.load(std::memory_order_acquire);
  for (size_t i = count; i > 0; --i) {
    FsoeConnection* connection = published_[i - 1];
    if (connection->config().slavePosition == slavePosition && connection->active()) {
      return connection;
    }
  }
  return nullptr;
}

std::vector<FsoeConnectionReport> FsoeManager::report() const {
  const size_t count = publishedCount_.load(std::memory_order_acquire);
  std::vector<FsoeConnectionReport> reports;
  reports.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const FsoeConnection* connection = published_[i];
    if (!connection->active()) {
      continue;  // retired by a close or by a re-open; it drives nothing.
    }
    reports.push_back(
        FsoeConnectionReport{.config = connection->config(), .state = connection->state()});
  }
  return reports;
}

void FsoeManager::execute() {
  const size_t count = publishedCount_.load(std::memory_order_acquire);
  if (count == 0) {
    lastCycle_.reset();
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const uint32_t dtUs =
      lastCycle_
          ? static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - *lastCycle_).count())
          : 0;
  lastCycle_ = now;

  for (size_t i = 0; i < count; ++i) {
    published_[i]->step(deviceManager_, dtUs);
  }
}

}  // namespace mm::node
