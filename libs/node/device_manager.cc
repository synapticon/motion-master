#include "node/device_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/seqlock.h"

namespace mm::node {

/// RT process-data runtime state (see the forward declaration in the header). The image
/// pointer is published with release / read with acquire; the seqlocks carry the flat output
/// and input images across the RT/non-RT boundary; the scratch buffers are touched only on
/// the RT thread. Published images are retained in @c generations so the RT thread never
/// reads a freed image — they are reclaimed only by reset(), when the loop is stopped.
struct ProcessData {
  std::atomic<const ProcessImage*> image{nullptr};
  std::vector<std::shared_ptr<const ProcessImage>> generations;
  mm::core::SeqLock<ProcessBuffer> outputStaging;
  mm::core::SeqLock<ProcessBuffer> inputSnapshot;
  ProcessBuffer outScratch;
  ProcessBuffer inScratch;
  // Serialises non-RT writers doing read-modify-write on outputStaging (the RT reader is
  // wait-free via the seqlock and does not take this lock).
  std::mutex stagingMutex;
  // Set by the RT thread while it is inside a driver exchange; lets stopExchange() drain an
  // in-flight cycle before a re-map or teardown mutates the IOmap.
  std::atomic<bool> exchanging{false};
  // Working-counter health. lastWkc is written by the RT thread each cycle; expectedWkc is
  // recomputed by the control plane from current device states. healthy = last >= expected.
  std::atomic<int> lastWkc{0};
  std::atomic<int> expectedWkc{0};
};

DeviceManager::DeviceManager() : pd_(std::make_unique<ProcessData>()) {}

DeviceManager::~DeviceManager() = default;

namespace {

/// Decodes a raw AL Status read-back into the API-facing DeviceStateInfo:
/// bits 3:0 are the state, bit 4 is the error indicator.
DeviceStateInfo decodeState(uint16_t slavePosition,
                            const mm::comm::FieldbusDriver::SlaveStateRaw& raw) {
  return {
      .slavePosition = slavePosition,
      .alStatus = raw.alStatus,
      .alState = static_cast<uint16_t>(raw.alStatus & 0x000Fu),
      .error = !!(raw.alStatus & 0x0010u),
      .alStatusCode = raw.alStatusCode,
  };
}

}  // namespace

std::expected<void, std::string> DeviceManager::init(
    std::unique_ptr<mm::comm::FieldbusDriver> driver) {
  // init() is a one-shot: replacing a live driver would destroy it while the
  // Devices in devices_ still hold a FieldbusDriver& to it, leaving every Device
  // with a dangling reference. Require an explicit reset() between inits instead.
  if (driver_) {
    return std::unexpected("already initialised — call reset() before init()");
  }
  driver_ = std::move(driver);
  auto result = driver_->init();
  if (!result) {
    // A failed init must leave us uninitialised — not holding a driver whose
    // context never opened. Otherwise initialised() would report true and the
    // next scan()/SDO call would dereference a null context. Dropping it here
    // also lets the caller simply retry init() without an intervening reset().
    spdlog::error("FieldbusDriver init failed: {}", result.error());
    driver_.reset();
  } else {
    spdlog::debug("FieldbusDriver initialised");
  }
  return result;
}

std::expected<int, std::string> DeviceManager::scan() {
  if (!driver_) {
    spdlog::error("scan() called with no driver — call init() first");
    return std::unexpected("no driver — call init() first");
  }
  auto result = driver_->scan();
  if (!result) {
    spdlog::error("FieldbusDriver scan failed: {}", result.error());
    return std::unexpected(result.error());
  }
  devices_.clear();
  for (uint16_t pos = 1; pos <= static_cast<uint16_t>(*result); ++pos) {
    devices_.emplace_back(pos, *driver_);
  }
  spdlog::info("Found {} slave(s)", *result);
  for (const auto& device : devices_) {
    spdlog::info("  [{:2}] {} — vendor: {:#010x}  product: {:#010x}  rev: {:#010x}  serial: {}",
                 device.slavePosition(), device.name(), device.vendorId(), device.productCode(),
                 device.revisionNumber(), device.serialNumber());
  }
  return *result;
}

void DeviceManager::reset() {
  // Unpublish the image and drain any in-flight cycle so a concurrent exchangeProcessData
  // becomes a no-op, then reclaim every retained image generation (safe now that exchange is
  // gated off).
  stopExchange();
  pd_->generations.clear();
  devices_.clear();  // drop device references to driver before stopping
  if (driver_) {
    driver_->stop();
    driver_.reset();
    spdlog::info("DeviceManager reset");
  }
}

const std::vector<Device>& DeviceManager::devices() const { return devices_; }

const Device* DeviceManager::findDevice(uint16_t slavePosition) const {
  auto it = std::find_if(devices_.begin(), devices_.end(), [slavePosition](const Device& d) {
    return d.slavePosition() == slavePosition;
  });
  return it != devices_.end() ? &*it : nullptr;
}

Device* DeviceManager::findDevice(uint16_t slavePosition) {
  auto it = std::find_if(devices_.begin(), devices_.end(), [slavePosition](const Device& d) {
    return d.slavePosition() == slavePosition;
  });
  return it != devices_.end() ? &*it : nullptr;
}

std::expected<void, std::string> DeviceManager::configureProcessData() {
  if (!driver_) {
    return std::unexpected("configureProcessData: no driver — call init() first");
  }
  if (devices_.empty()) {
    return std::unexpected("configureProcessData: no devices — call scan() first");
  }
  // Unpublish first and drain any in-flight cycle so the RT thread is not touching the IOmap
  // while we re-map it.
  stopExchange();

  if (auto r = driver_->configureProcessData(); !r) {
    return std::unexpected(r.error());
  }
  for (auto& device : devices_) {
    if (auto r = device.readPdoMappings(); !r) {
      return std::unexpected(r.error());
    }
  }
  auto image = buildProcessImage(driver_->processDataLayout(), devices_);
  if (!image) {
    return std::unexpected(image.error());
  }

  // Seed the output staging from the current cached parameter values, so the RxPDO setpoints
  // callers initialised before OP (controlword, modes, targets) are sent on the very first
  // cycle. Unset parameters encode their type-appropriate zero, which is the safe default.
  ProcessBuffer staging;
  staging.size = image->outputBytes;
  for (const auto& entry : image->outputs) {
    const Device* device = findDevice(entry.slavePosition);
    if (!device) {
      continue;
    }
    const DeviceParameter* p = device->parameter(entry.index, entry.subindex);
    if (!p) {
      continue;
    }
    auto bytes = encodeSdoBytes(p->dataType, p->value);
    if (!bytes) {
      continue;
    }
    insertBits(std::span<uint8_t>(staging.bytes.data(), staging.size), entry.bitOffset,
               entry.bitLength, *bytes);
  }
  pd_->outputStaging.store(staging);

  auto shared = std::make_shared<const ProcessImage>(std::move(*image));
  pd_->generations.push_back(shared);
  pd_->image.store(shared.get(), std::memory_order_release);
  updateExpectedWkc();
  spdlog::info("Process data configured: {} output bytes, {} input bytes, expected WKC {}",
               shared->outputBytes, shared->inputBytes, shared->expectedWkc);
  return {};
}

void DeviceManager::exchangeProcessData() {
  const ProcessImage* image = pd_->image.load(std::memory_order_acquire);
  if (!driver_ || image == nullptr) {
    return;  // not mapped yet — safe no-op so the GameLoop can call this every cycle
  }
  // Mark the cycle in flight so stopExchange() can drain it before a re-map/teardown.
  pd_->exchanging.store(true, std::memory_order_release);
  // Load staged outputs, exchange one cycle, publish the received inputs. The scratch buffers
  // are touched only on this (RT) thread; the seqlocks hand data to/from non-RT readers.
  pd_->outputStaging.load(pd_->outScratch);
  const int wkc = driver_->exchangeProcessData(
      std::span<const uint8_t>(pd_->outScratch.bytes.data(), image->outputBytes),
      std::span<uint8_t>(pd_->inScratch.bytes.data(), image->inputBytes));
  pd_->inScratch.size = image->inputBytes;
  pd_->inputSnapshot.store(pd_->inScratch);
  // Publish the working counter for health checks; expectedWkc is maintained off the RT path.
  pd_->lastWkc.store(wkc, std::memory_order_relaxed);
  pd_->exchanging.store(false, std::memory_order_release);
}

void DeviceManager::stopExchange() {
  pd_->image.store(nullptr, std::memory_order_release);
  // Drain an in-flight exchange cycle: once the image is null the RT thread will not start a
  // new one, so we only wait out the at-most-one cycle already past the null check. Bounded so
  // a stalled/absent RT loop can never hang a control-plane call.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (pd_->exchanging.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
}

ProcessBuffer DeviceManager::inputSnapshot() const { return pd_->inputSnapshot.load(); }

bool DeviceManager::processDataConfigured() const {
  return pd_->image.load(std::memory_order_acquire) != nullptr;
}

int DeviceManager::lastWorkingCounter() const {
  return pd_->lastWkc.load(std::memory_order_relaxed);
}

int DeviceManager::expectedWorkingCounter() const {
  return pd_->expectedWkc.load(std::memory_order_relaxed);
}

bool DeviceManager::processDataHealthy() const {
  return processDataConfigured() && pd_->lastWkc.load(std::memory_order_relaxed) >=
                                        pd_->expectedWkc.load(std::memory_order_relaxed);
}

void DeviceManager::updateExpectedWkc() {
  using mm::comm::EtherCatState;
  // SOEM's working-counter model: per output-mapped slave +2, per input-mapped slave +1. In
  // SAFE-OP only the input sync manager is active, so outputs do not count; in PRE-OP/below a
  // slave does not contribute at all. Summing over current states yields the WKC expected from
  // a partially-operational bus — the figure a health check must compare against.
  int expected = 0;
  if (driver_) {
    for (const auto& device : devices_) {
      const uint16_t status = driver_->slaveState(device.slavePosition());
      if (status & 0x0010u) {
        continue;  // error indicator set — treat as not contributing
      }
      const uint16_t state = status & 0x000Fu;
      const bool hasOutputs = device.pdoMappings().outputBits > 0;
      const bool hasInputs = device.pdoMappings().inputBits > 0;
      if (state == static_cast<uint16_t>(EtherCatState::Op)) {
        expected += (hasOutputs ? 2 : 0) + (hasInputs ? 1 : 0);
      } else if (state == static_cast<uint16_t>(EtherCatState::SafeOp)) {
        expected += hasInputs ? 1 : 0;
      }
    }
  }
  pd_->expectedWkc.store(expected, std::memory_order_relaxed);
}

std::expected<std::vector<DeviceStateInfo>, std::string> DeviceManager::transitionToState(
    const std::vector<uint16_t>& positions, mm::comm::EtherCatState targetState,
    std::chrono::steady_clock::duration timeout) {
  if (!driver_) {
    return std::unexpected("no driver — call init() first");
  }
  if (devices_.empty()) {
    return std::unexpected("no devices — call scan() first");
  }
  std::vector<uint16_t> targets = positions;
  if (targets.empty()) {
    targets.reserve(devices_.size());
    std::transform(devices_.begin(), devices_.end(), std::back_inserter(targets),
                   [](const Device& d) { return d.slavePosition(); });
  }

  // React to the requested state. Process data is exchanged only in SAFE-OP and OP, so Motion
  // Master configures or tears down the mapping around the user-driven AL transition:
  //  - entering SAFE-OP/OP without a published image (first time, or after a device returned
  //    from a firmware download): map and publish now, while the mailbox is up in PRE-OP, so
  //    FMMUs are programmed before the transition and exchange can resume on the next cycle;
  //  - leaving the exchanging states (down to PRE-OP/BOOT, e.g. for a download): stop exchange
  //    and drop the image. Under the whole-bus model this pauses exchange for every device.
  const bool exchangeState =
      targetState == mm::comm::EtherCatState::SafeOp || targetState == mm::comm::EtherCatState::Op;
  if (exchangeState && !processDataConfigured()) {
    if (auto r = configureProcessData(); !r) {
      return std::unexpected("auto-configure process data failed: " + r.error());
    }
  } else if (!exchangeState && processDataConfigured()) {
    spdlog::info("Stopping process data exchange before transition to 0x{:02X}",
                 static_cast<int>(targetState));
    stopExchange();
  }

  spdlog::debug("transitionToState -> 0x{:02X} for {} device(s)", static_cast<int>(targetState),
                targets.size());
  driver_->transitionToState(targets, std::nullopt, targetState, timeout);

  // The driver call only logs failures, so read the settled state back and return it.
  // Callers derive "reached the target" as (!error && alState == targetState) per device.
  auto raw = driver_->readStates(targets);
  if (!raw) {
    return std::unexpected(raw.error());
  }
  // readStates() refreshed the driver's cached AL status for every slave, so Device::online()
  // / exchangesProcessData() now read through to the fresh state — no per-device copy to sync.
  std::vector<DeviceStateInfo> result;
  result.reserve(targets.size());
  for (std::size_t i = 0; i < targets.size(); ++i) {
    result.push_back(decodeState(targets[i], (*raw)[i]));
  }
  // Device states just changed, so the working counter expected from the bus did too.
  updateExpectedWkc();

  // Reconcile module idents once a device reaches PRE-OP. A modular drive whose
  // Configured Module Ident List (0xF030) disagrees with its Detected list (0xF050)
  // reports a mismatch and refuses to leave PRE-OP; copying detected into configured
  // clears it. SDO mailbox is only available in PRE-OP+, so this is the earliest point
  // the write is possible. Best-effort: failures are logged, never fatal to the transition.
  if (targetState == mm::comm::EtherCatState::PreOp) {
    for (const auto& info : result) {
      if (info.error || info.alState != static_cast<uint16_t>(mm::comm::EtherCatState::PreOp)) {
        continue;
      }
      const Device* device = findDevice(info.slavePosition);
      if (!device) {
        continue;
      }
      auto reconciled = reconcileDetectedModules(*device);
      if (!reconciled) {
        spdlog::warn("Device {}: module ident reconcile failed: {}", info.slavePosition,
                     reconciled.error());
      } else if (*reconciled > 0) {
        spdlog::info("Device {}: reconciled {} module slot(s)", info.slavePosition, *reconciled);
      }
    }
  }

  return result;
}

void to_json(nlohmann::json& j, const DeviceStateInfo& info) {
  j = {{"slavePosition", info.slavePosition},
       {"alStatus", info.alStatus},
       {"alState", info.alState},
       {"error", info.error},
       {"alStatusCode", info.alStatusCode}};
}

std::expected<std::vector<DeviceStateInfo>, std::string> DeviceManager::getDeviceStates(
    const std::vector<uint16_t>& positions) {
  if (!driver_) {
    return std::unexpected("no driver — call init() first");
  }
  std::vector<uint16_t> targets = positions;
  if (targets.empty()) {
    targets.reserve(devices_.size());
    std::transform(devices_.begin(), devices_.end(), std::back_inserter(targets),
                   [](const Device& d) { return d.slavePosition(); });
  }
  auto raw = driver_->readStates(targets);
  if (!raw) {
    return std::unexpected(raw.error());
  }
  std::vector<DeviceStateInfo> result;
  result.reserve(targets.size());
  for (std::size_t i = 0; i < targets.size(); ++i) {
    result.push_back(decodeState(targets[i], (*raw)[i]));
  }
  return result;
}

std::expected<bool, std::string> DeviceManager::isDeviceOnline(uint16_t slavePosition) {
  if (!driver_) {
    return std::unexpected("no driver — call init() first");
  }
  const Device* device = findDevice(slavePosition);
  if (!device) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  // Reading the single position refreshes the driver's cached AL status, which Device::online()
  // reads through — reusing the one place that defines "online" rather than duplicating it.
  if (auto states = getDeviceStates({slavePosition}); !states) {
    return std::unexpected(states.error());
  }
  return device->online();
}

std::expected<void, std::string> DeviceManager::initializeDeviceParameters(uint16_t slavePosition,
                                                                           bool readValues) {
  auto it = std::find_if(devices_.begin(), devices_.end(), [slavePosition](const Device& d) {
    return d.slavePosition() == slavePosition;
  });
  if (it == devices_.end()) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  return it->initializeParameters(readValues);
}

std::optional<std::vector<uint8_t>> DeviceManager::readPdoValue(uint16_t slavePosition,
                                                                uint16_t index,
                                                                uint8_t subindex) const {
  const ProcessImage* image = pd_->image.load(std::memory_order_acquire);
  if (!image) {
    return std::nullopt;
  }
  auto loc = image->find(slavePosition, index, subindex);
  if (!loc) {
    return std::nullopt;
  }
  // Inputs live in the latest snapshot; outputs in the staging buffer (what is being sent).
  const ProcessBuffer buffer =
      loc->isOutput ? pd_->outputStaging.load() : pd_->inputSnapshot.load();
  return extractBits(std::span<const uint8_t>(buffer.bytes.data(), buffer.size), loc->bitOffset,
                     loc->bitLength);
}

bool DeviceManager::writePdoValue(uint16_t slavePosition, uint16_t index, uint8_t subindex,
                                  std::span<const uint8_t> bytes) {
  const ProcessImage* image = pd_->image.load(std::memory_order_acquire);
  if (!image) {
    return false;
  }
  auto loc = image->find(slavePosition, index, subindex);
  if (!loc || !loc->isOutput) {
    return false;  // only outputs are writable from the master
  }
  // Read-modify-write the staging buffer; serialise against other non-RT writers.
  std::lock_guard<std::mutex> lock(pd_->stagingMutex);
  ProcessBuffer buffer;
  pd_->outputStaging.load(buffer);
  insertBits(std::span<uint8_t>(buffer.bytes.data(), buffer.size), loc->bitOffset, loc->bitLength,
             bytes);
  pd_->outputStaging.store(buffer);
  return true;
}

std::expected<DeviceParameterValue, std::string> DeviceManager::readDeviceParameter(
    uint16_t slavePosition, uint16_t index, uint8_t subindex) {
  Device* device = findDevice(slavePosition);
  if (!device) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  // Use the process image only for a device actually exchanging (SAFE-OP/OP). A device left in
  // PRE-OP on a partially-operational bus is in the image but not filling its region, so its
  // value must come over SDO — not from the stale buffer.
  if (device->exchangesProcessData()) {
    // The live value lives in the process data — decode it and reflect it into the cached
    // DeviceParameter (which stays the source of truth).
    if (auto bytes = readPdoValue(slavePosition, index, subindex); bytes) {
      const DeviceParameter* p = device->parameter(index, subindex);
      if (!p) {
        return std::unexpected(std::format("device {}: parameter 0x{:04X}:{:02X} not found",
                                           slavePosition, index, subindex));
      }
      auto decoded = decodeSdoBytes(p->dataType, *bytes);
      if (!decoded) {
        return std::unexpected(decoded.error());
      }
      if (auto set = device->setCachedValue(index, subindex, *decoded); !set) {
        return std::unexpected(set.error());
      }
      return *decoded;
    }
  }
  return device->readParameter(index, subindex);
}

std::expected<void, std::string> DeviceManager::writeDeviceParameter(uint16_t slavePosition,
                                                                     uint16_t index,
                                                                     uint8_t subindex,
                                                                     DeviceParameterValue value) {
  Device* device = findDevice(slavePosition);
  if (!device) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  // When the object is a PDO output of a device that is exchanging (SAFE-OP/OP), coerce +
  // cache it and stage it into the output buffer (sent every cycle) instead of an SDO download.
  // A device not exchanging falls through to SDO, even if it appears in the published image.
  const ProcessImage* image = pd_->image.load(std::memory_order_acquire);
  if (image && device->exchangesProcessData()) {
    if (auto loc = image->find(slavePosition, index, subindex); loc && loc->isOutput) {
      if (auto set = device->setCachedValue(index, subindex, std::move(value)); !set) {
        return std::unexpected(set.error());
      }
      const DeviceParameter* p = device->parameter(index, subindex);
      auto bytes = encodeSdoBytes(p->dataType, p->value);
      if (!bytes) {
        return std::unexpected(bytes.error());
      }
      writePdoValue(slavePosition, index, subindex, *bytes);
      return {};
    }
  }
  return device->writeParameter(index, subindex, std::move(value));
}

void to_json(nlohmann::json& j, const DeviceManager& dm) { j = dm.devices(); }

}  // namespace mm::node
