#include "node/device_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <format>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/seqlock.h"
#include "node/process_data.h"

namespace mm::node {

// ProcessData (the live process-data runtime: published image pointer + exchange seqlocks +
// working-counter health) is defined in node/process_data.h so device.cc can use it too — a Device
// reaches its live IOmap objects through ProcessData::readPdo / writePdo. Those two accessors are
// defined out-of-line below, after the bit/byte helpers they use. Published images are retained in
// ProcessData::generations until reset() so a lock-free reader never dereferences a freed image.

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
      .alState = static_cast<uint16_t>(mm::comm::alState(raw.alStatus)),
      .error = mm::comm::alHasError(raw.alStatus),
      .alStatusCode = raw.alStatusCode,
  };
}

/// Number of leading bytes of a ProcessBuffer that are live for an image of @p imageBytes: the
/// size field plus the active byte prefix. The process-data seqlocks copy only these, so the
/// per-cycle traffic is proportional to the real image (a few hundred bytes) rather than the full
/// kMaxProcessImageBytes capacity (~32 KB). @p imageBytes is bounded by configureProcessData, and
/// the seqlock clamps the count to sizeof(ProcessBuffer) regardless.
size_t liveBufferBytes(uint32_t imageBytes) { return offsetof(ProcessBuffer, bytes) + imageBytes; }

}  // namespace

std::expected<void, std::string> DeviceManager::init(
    std::unique_ptr<mm::comm::FieldbusDriver> driver) {
  std::unique_lock lock(busMutex_);
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
  std::unique_lock lock(busMutex_);
  if (!driver_) {
    spdlog::error("scan() called with no driver — call init() first");
    return std::unexpected("no driver — call init() first");
  }
  // A re-scan reprograms the whole SOEM context (ecx_config_init rebuilds the slavelist, sync
  // managers, and FMMUs) and replaces the device set, invalidating the current process image.
  // Unpublish and drain the RT cycle first — exactly as reset()/configureProcessData() do — so
  // exchangeProcessData() is a no-op and the RT loop is no longer touching the IOmap while the
  // driver rebuilds it. Then reclaim the now-stale image generations (safe once exchange is
  // gated off); a fresh image is published when the bus is next brought into SAFE-OP/OP.
  stopExchange();
  pd_->generations.clear();
  auto result = driver_->scan();
  if (!result) {
    spdlog::error("FieldbusDriver scan failed: {}", result.error());
    return std::unexpected(result.error());
  }
  devices_.clear();
  for (uint16_t pos = 1; pos <= static_cast<uint16_t>(*result); ++pos) {
    // Hand each device the process-data runtime so its read/writeParameter can serve the live
    // IOmap value while exchanging (and stage outputs), falling back to SDO otherwise. pd_ is
    // created once in the constructor and never replaced, so the pointer is stable for our
    // lifetime.
    devices_.emplace_back(pos, *driver_, pd_.get());
  }
  // The device set was rebuilt: positions may now name different devices. Bump the generation
  // so off-thread consumers (monitoring) that pinned to a position re-validate it.
  topologyGeneration_.fetch_add(1, std::memory_order_relaxed);
  spdlog::info("Found {} slave(s)", *result);
  for (const auto& device : devices_) {
    spdlog::info("  [{:2}] {} — vendor: {:#010x}  product: {:#010x}  rev: {:#010x}  serial: {}",
                 device.slavePosition(), device.name(), device.vendorId(), device.productCode(),
                 device.revisionNumber(), device.serialNumber());
  }
  return *result;
}

void DeviceManager::reset() {
  std::unique_lock lock(busMutex_);
  // Unpublish the image and drain any in-flight cycle so a concurrent exchangeProcessData
  // becomes a no-op, then reclaim every retained image generation (safe now that exchange is
  // gated off).
  stopExchange();
  pd_->generations.clear();
  devices_.clear();  // drop device references to driver before stopping
  // The device set is gone: bump the generation so off-thread consumers re-validate (and find
  // their positions no longer resolve).
  topologyGeneration_.fetch_add(1, std::memory_order_relaxed);
  if (driver_) {
    driver_->stop();
    driver_.reset();
    spdlog::info("DeviceManager reset");
  }
}

uint64_t DeviceManager::topologyGeneration() const {
  return topologyGeneration_.load(std::memory_order_relaxed);
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
  std::unique_lock lock(busMutex_);
  return remapProcessImage();
}

std::expected<void, std::string> DeviceManager::remapProcessImage() {
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
  // A new image is published: object offsets may differ from the previous one, so signal
  // consumers that captured the layout (the monitoring sampler) to re-capture it.
  processImageGeneration_.fetch_add(1, std::memory_order_relaxed);
  updateExpectedWkc();
  spdlog::info("Process data configured: {} output bytes, {} input bytes, expected WKC {}",
               shared->outputBytes, shared->inputBytes, shared->expectedWkc);
  return {};
}

void DeviceManager::exchangeProcessData() {
  if (!driver_) {
    return;  // no driver — safe no-op so the GameLoop can call this every cycle
  }
  // Raise the in-flight flag BEFORE reading the published image, then re-read the image: this
  // closes the race against stopExchange(), which stores nullptr and then waits on this flag.
  // Both the flag store and the image load are sequentially consistent so they cannot be
  // reordered against stopExchange()'s image store / flag load (a StoreLoad pair that only
  // seq_cst prevents). The total order then guarantees that for any concurrent teardown either
  // we observe the null image and back out here, or stopExchange() observes the flag and drains
  // us — never both missing each other and letting us touch a half-remapped IOmap.
  pd_->exchanging.store(true, std::memory_order_seq_cst);
  const ProcessImage* image = pd_->image.load(std::memory_order_seq_cst);
  if (image == nullptr) {
    pd_->exchanging.store(false, std::memory_order_release);
    return;  // not mapped yet, or torn down mid-flight — back out without touching the IOmap
  }
  // Load staged outputs, exchange one cycle, publish the received inputs. The scratch buffers
  // are touched only on this (RT) thread; the seqlocks hand data to/from non-RT readers. Both
  // copies move only the live prefix (the published image size, which is stable) rather than the
  // full kMaxProcessImageBytes capacity, keeping per-cycle memory traffic proportional to the
  // real image.
  pd_->outputStaging.load(pd_->outScratch, liveBufferBytes(image->outputBytes));
  const int wkc = driver_->exchangeProcessData(
      std::span<const uint8_t>(pd_->outScratch.bytes.data(), image->outputBytes),
      std::span<uint8_t>(pd_->inScratch.bytes.data(), image->inputBytes));
  pd_->inScratch.size = image->inputBytes;
  // Always publish the latest snapshot. On a lost or partial frame the driver leaves the prior
  // bytes in the IOmap, so this may republish stale data — readers gate on the working counter
  // below (processDataHealthy()) rather than here, since skipping the store would only leave an
  // even older snapshot in the seqlock and still expose nothing about freshness.
  pd_->inputSnapshot.store(pd_->inScratch, liveBufferBytes(image->inputBytes));
  // Publish the working counter for health checks; expectedWkc is maintained off the RT path.
  pd_->lastWkc.store(wkc, std::memory_order_relaxed);
  pd_->exchanging.store(false, std::memory_order_release);
}

void DeviceManager::stopExchange() {
  pd_->image.store(nullptr, std::memory_order_seq_cst);
  // Drain an in-flight exchange cycle. Both operations here are sequentially consistent so they
  // pair with exchangeProcessData()'s seq_cst flag-store / image-load: once we have stored the
  // null image, any RT cycle that has already raised the flag is visible to this load, and any
  // RT cycle that has not yet raised it will observe the null image and back out. We therefore
  // only wait out the at-most-one cycle already in flight. Bounded so a stalled/absent RT loop
  // can never hang a control-plane call.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (pd_->exchanging.load(std::memory_order_seq_cst) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
}

ProcessBuffer DeviceManager::inputSnapshot() const { return pd_->inputSnapshot.load(); }

ProcessBuffer DeviceManager::outputSnapshot() const { return pd_->outputStaging.load(); }

bool DeviceManager::processDataConfigured() const {
  return pd_->image.load(std::memory_order_acquire) != nullptr;
}

uint64_t DeviceManager::processImageGeneration() const {
  return processImageGeneration_.load(std::memory_order_relaxed);
}

int DeviceManager::lastWorkingCounter() const {
  return pd_->lastWkc.load(std::memory_order_relaxed);
}

int DeviceManager::expectedWorkingCounter() const {
  return pd_->expectedWkc.load(std::memory_order_relaxed);
}

bool DeviceManager::processDataHealthy() const { return pd_->healthy(); }

ProcessImageInfo DeviceManager::processImageInfo() const {
  ProcessImageInfo info{};
  info.lastWkc = pd_->lastWkc.load(std::memory_order_relaxed);
  info.expectedWkc = pd_->expectedWkc.load(std::memory_order_relaxed);
  // generations is mutated only by configureProcessData()/reset(), both on the control-plane
  // thread that also calls this accessor — so the size read needs no synchronisation.
  info.generations = pd_->generations.size();

  const ProcessImage* image = pd_->image.load(std::memory_order_acquire);
  info.configured = image != nullptr;
  info.healthy = info.configured && info.lastWkc >= info.expectedWkc;

  // Describe the live image when exchanging; otherwise fall back to the most recent retained
  // generation so a bus that has dropped out of SAFE-OP/OP (image torn down, but generations
  // kept until reset()) still shows what it last mapped. `configured` tells the caller which it
  // is, so a stale layout is never mistaken for an active one.
  const ProcessImage* describe = image;
  if (describe == nullptr && !pd_->generations.empty()) {
    describe = pd_->generations.back().get();
  }
  if (describe == nullptr) {
    return info;
  }
  info.outputBytes = describe->outputBytes;
  info.inputBytes = describe->inputBytes;

  // Resolve each entry's name from the owning device's parameter map (empty when its object
  // dictionary has not been enumerated — PDO mappings are read independently of OD enumeration).
  auto flatten = [this](const std::vector<ProcessImageEntry>& entries) {
    std::vector<ProcessImageObjectInfo> out;
    out.reserve(entries.size());
    for (const auto& e : entries) {
      std::string name;
      if (const Device* device = findDevice(e.slavePosition)) {
        if (const DeviceParameter* p = device->parameter(e.index, e.subindex)) {
          name = p->name;
        }
      }
      out.push_back(
          {e.slavePosition, e.index, e.subindex, std::move(name), e.bitOffset, e.bitLength});
    }
    return out;
  };
  info.outputs = flatten(describe->outputs);
  info.inputs = flatten(describe->inputs);
  return info;
}

std::vector<SlaveConfigInfo> DeviceManager::busConfig() const {
  std::vector<SlaveConfigInfo> out;
  if (!driver_) {
    return out;
  }
  auto configs = driver_->busConfig();
  out.reserve(configs.size());
  for (auto& c : configs) {
    std::string name;
    if (const Device* device = findDevice(c.slavePosition)) {
      name = device->name();
    }
    out.push_back(SlaveConfigInfo{.config = std::move(c), .deviceName = std::move(name)});
  }
  return out;
}

std::expected<std::vector<uint16_t>, std::string> DeviceManager::resolveTargets(
    const std::vector<uint16_t>& positions) const {
  if (positions.empty()) {
    std::vector<uint16_t> all;
    all.reserve(devices_.size());
    std::transform(devices_.begin(), devices_.end(), std::back_inserter(all),
                   [](const Device& d) { return d.slavePosition(); });
    return all;
  }
  // Validate every caller-supplied position against the device set before it reaches the
  // driver: the slave-indexed driver accessors (slaveState, readStates, readDiagnostics,
  // readDcSync, transitionToState) index a fixed-size slavelist without bounds-checking, so an
  // unknown/out-of-range position would be an out-of-bounds read (and, for a state change, a
  // write to a bogus station). Reject it here, mirroring the 404 the single-device routes give.
  auto unknown =
      std::ranges::find_if(positions, [this](uint16_t pos) { return findDevice(pos) == nullptr; });
  if (unknown != positions.end()) {
    return std::unexpected("unknown device position " + std::to_string(*unknown));
  }
  return positions;
}

void DeviceManager::updateExpectedWkc() {
  // Sum each non-errored device's working-counter contribution for its current AL state and PDO
  // presence. The protocol rule (how outputs/inputs and SAFE-OP/OP map to a WKC increment) lives
  // in the comm layer; here we only know each device's live state and whether it maps any PDO, so
  // the figure tracks a partially-operational bus — what a health check compares against.
  int expected = 0;
  if (driver_) {
    for (const auto& device : devices_) {
      const uint16_t status = driver_->slaveState(device.slavePosition());
      if (mm::comm::alHasError(status)) {
        continue;  // error indicator set — treat as not contributing
      }
      expected += mm::comm::workingCounterContribution(mm::comm::alState(status),
                                                       device.pdoMappings().outputBits > 0,
                                                       device.pdoMappings().inputBits > 0);
    }
  }
  pd_->expectedWkc.store(expected, std::memory_order_relaxed);
}

std::expected<std::vector<DeviceStateInfo>, std::string> DeviceManager::transitionToState(
    const std::vector<uint16_t>& positions, mm::comm::EtherCatState targetState,
    std::chrono::steady_clock::duration timeout) {
  std::unique_lock lock(busMutex_);
  if (!driver_) {
    return std::unexpected("no driver — call init() first");
  }
  if (devices_.empty()) {
    return std::unexpected("no devices — call scan() first");
  }
  auto resolved = resolveTargets(positions);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  std::vector<uint16_t> targets = std::move(*resolved);

  // React to the requested state. Process data is exchanged only in SAFE-OP and OP, so Motion
  // Master maps/re-maps or tears down the whole-bus image around the user-driven AL transition.
  // A subset of the bus can be taken down (firmware download in BOOT, or a manual PDO re-map in
  // PRE-OP) while the devices staying in SAFE-OP/OP keep exchanging; a device rejoining triggers
  // a re-map because its firmware may carry a different PDO layout.
  const bool exchangeState =
      targetState == mm::comm::EtherCatState::SafeOp || targetState == mm::comm::EtherCatState::Op;
  if (exchangeState) {
    // Entering an exchange state. Re-map when there is no image yet, or when any targeted device
    // is (re)joining from a non-exchange state — it must be added to the whole-bus image and its
    // PDO mapping re-read (a firmware update or manual re-map may have changed it). A device
    // already exchanging being re-commanded (SAFE-OP -> OP) needs no re-map; the published image
    // still describes it. Re-mapping briefly pauses exchange for the whole bus (stopExchange
    // inside configureProcessData) — the accepted cost of bringing a device back online.
    const bool anyJoining = std::ranges::any_of(targets, [this](uint16_t pos) {
      const Device* d = findDevice(pos);
      return d && !d->exchangesProcessData();
    });
    if (!processDataConfigured() || anyJoining) {
      // Re-mapping rebuilds the single whole-bus IOmap in one shot, so remapProcessImage()
      // pauses PDO for *every* device — including ones meant to stay in OP — for the duration
      // of stopExchange() plus the per-device SDO reads it does. A device in OP that misses
      // process data past its sync-manager watchdog timeout faults itself to SAFE-OP+error. To
      // avoid that, deliberately drop any staying-in-OP device to SAFE-OP first (a missed frame
      // in SAFE-OP is harmless — outputs are not applied there), re-map, then climb them back to
      // OP. The GameLoop resumes feeding PDO the instant the new image is published, so the climb
      // back is fed normally and needs no separate PDO tick.
      std::vector<uint16_t> opStayers;
      for (const Device& d : devices_) {
        const bool targeted = std::ranges::find(targets, d.slavePosition()) != targets.end();
        if (!targeted && mm::comm::alState(driver_->slaveState(d.slavePosition())) ==
                             mm::comm::EtherCatState::Op) {
          opStayers.push_back(d.slavePosition());
        }
      }
      if (!opStayers.empty()) {
        spdlog::info(
            "Re-map pauses the whole bus — dropping {} staying OP device(s) to SAFE-OP first to "
            "avoid a sync-manager watchdog fault",
            opStayers.size());
        driver_->transitionToState(opStayers, std::nullopt, mm::comm::EtherCatState::SafeOp,
                                   timeout);
      }

      // Already holding busMutex_ exclusively — call the mapping primitive directly, not the
      // public configureProcessData (which would re-acquire the non-recursive lock and deadlock).
      if (auto r = remapProcessImage(); !r) {
        return std::unexpected("auto-configure process data failed: " + r.error());
      }

      // Restore the staying devices to OP. The freshly published image already describes them, so
      // no further re-map is needed; the running GameLoop feeds PDO during the wait.
      if (!opStayers.empty()) {
        driver_->transitionToState(opStayers, mm::comm::EtherCatState::SafeOp,
                                   mm::comm::EtherCatState::Op, timeout);
      }
    }
  } else if (processDataConfigured()) {
    // Leaving the exchange states. Tear the whole-bus image down only when no device will remain
    // exchanging afterwards; otherwise keep it published so the devices staying in SAFE-OP/OP
    // keep running while the targeted ones drop out. The targeted devices' working-counter share
    // is removed by the updateExpectedWkc() below, so health still reflects the live bus.
    const bool anyStays = std::ranges::any_of(devices_, [&targets](const Device& d) {
      const bool targeted = std::ranges::find(targets, d.slavePosition()) != targets.end();
      return !targeted && d.exchangesProcessData();
    });
    if (anyStays) {
      spdlog::info(
          "Transition to 0x{:02X} leaves other devices in SAFE-OP/OP — keeping the process image",
          static_cast<int>(targetState));
    } else {
      spdlog::info("Stopping process data exchange before transition to 0x{:02X}",
                   static_cast<int>(targetState));
      stopExchange();
    }
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
  auto resolved = resolveTargets(positions);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  std::vector<uint16_t> targets = std::move(*resolved);
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

std::expected<std::vector<DeviceDiagnosticsInfo>, std::string> DeviceManager::getDeviceDiagnostics(
    const std::vector<uint16_t>& positions) {
  if (!driver_) {
    return std::unexpected("no driver — call init() first");
  }
  auto resolved = resolveTargets(positions);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  std::vector<uint16_t> targets = std::move(*resolved);
  auto raw = driver_->readDiagnostics(targets);
  if (!raw) {
    return std::unexpected(raw.error());
  }
  std::vector<DeviceDiagnosticsInfo> result;
  result.reserve(raw->size());
  for (auto& d : *raw) {
    std::string name;
    if (const Device* device = findDevice(d.slavePosition)) {
      name = device->name();
    }
    result.push_back(DeviceDiagnosticsInfo{.diagnostics = d, .deviceName = std::move(name)});
  }
  return result;
}

std::expected<std::vector<DcSyncInfo>, std::string> DeviceManager::getDcSync(
    const std::vector<uint16_t>& positions) {
  if (!driver_) {
    return std::unexpected("no driver — call init() first");
  }
  auto resolved = resolveTargets(positions);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  std::vector<uint16_t> targets = std::move(*resolved);
  auto raw = driver_->readDcSync(targets);
  if (!raw) {
    return std::unexpected(raw.error());
  }
  std::vector<DcSyncInfo> result;
  result.reserve(raw->size());
  for (auto& d : *raw) {
    std::string name;
    if (const Device* device = findDevice(d.slavePosition)) {
      name = device->name();
    }
    result.push_back(DcSyncInfo{.dcSync = d, .deviceName = std::move(name)});
  }
  return result;
}

std::expected<mm::comm::ProcessDataWatchdogConfig, std::string>
DeviceManager::getProcessDataWatchdog(uint16_t slavePosition) {
  std::shared_lock lock(busMutex_);
  if (!driver_) {
    return std::unexpected("no driver — call init() first");
  }
  if (!findDevice(slavePosition)) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  return driver_->processDataWatchdog(slavePosition);
}

std::expected<mm::comm::ProcessDataWatchdogConfig, std::string>
DeviceManager::setProcessDataWatchdog(uint16_t slavePosition, std::chrono::nanoseconds timeout) {
  std::shared_lock lock(busMutex_);
  if (!driver_) {
    return std::unexpected("no driver — call init() first");
  }
  if (!findDevice(slavePosition)) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  return driver_->setProcessDataWatchdog(slavePosition, timeout);
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

std::optional<std::vector<uint8_t>> ProcessData::readPdo(uint16_t slavePosition, uint16_t index,
                                                         uint8_t subindex) const {
  // Gate on health: on a lost or partial frame the driver leaves the prior cycle's bytes in the
  // IOmap, so the snapshot is stale — signal the caller to read the authoritative SDO value instead
  // (a healthy() check also covers "no image published").
  if (!healthy()) {
    return std::nullopt;
  }
  const ProcessImage* img = image.load(std::memory_order_acquire);
  auto loc = img->find(slavePosition, index, subindex);
  if (!loc) {
    return std::nullopt;
  }
  // Inputs live in the latest snapshot; outputs in the staging buffer (what is being sent). Copy
  // only the live prefix (the published image size) rather than the full capacity.
  const size_t live = liveBufferBytes(loc->isOutput ? img->outputBytes : img->inputBytes);
  ProcessBuffer buffer;
  if (loc->isOutput) {
    outputStaging.load(buffer, live);
  } else {
    inputSnapshot.load(buffer, live);
  }
  return extractBits(std::span<const uint8_t>(buffer.bytes.data(), buffer.size), loc->bitOffset,
                     loc->bitLength);
}

bool ProcessData::writePdo(uint16_t slavePosition, uint16_t index, uint8_t subindex,
                           std::span<const uint8_t> bytes) {
  const ProcessImage* img = image.load(std::memory_order_acquire);
  if (!img) {
    return false;
  }
  auto loc = img->find(slavePosition, index, subindex);
  if (!loc || !loc->isOutput) {
    return false;  // only outputs are writable from the master
  }
  // Read-modify-write the staging buffer; serialise against other non-RT writers. Copy only the
  // live prefix (the published output image size) rather than the full capacity.
  const size_t live = liveBufferBytes(img->outputBytes);
  std::lock_guard<std::mutex> lock(stagingMutex);
  ProcessBuffer buffer;
  outputStaging.load(buffer, live);
  insertBits(std::span<uint8_t>(buffer.bytes.data(), buffer.size), loc->bitOffset, loc->bitLength,
             bytes);
  outputStaging.store(buffer, live);
  return true;
}

std::optional<DeviceParameterValue> DeviceManager::value(uint16_t slavePosition, uint16_t index,
                                                         uint8_t subindex) const {
  std::shared_lock lock(busMutex_);
  const Device* device = findDevice(slavePosition);
  if (!device) {
    return std::nullopt;
  }
  return device->value(index, subindex);
}

std::optional<DeviceManager::PdoSampleSpec> DeviceManager::pdoSampleSpec(uint16_t slavePosition,
                                                                         uint16_t index,
                                                                         uint8_t subindex) const {
  std::shared_lock lock(busMutex_);
  const ProcessImage* image = pd_->image.load(std::memory_order_acquire);
  if (!image) {
    return std::nullopt;
  }
  auto loc = image->find(slavePosition, index, subindex);
  if (!loc) {
    return std::nullopt;  // not PDO-mapped in the published image
  }
  const Device* device = findDevice(slavePosition);
  if (!device) {
    return std::nullopt;
  }
  auto dataType = device->dataType(index, subindex);
  if (!dataType) {
    return std::nullopt;  // object dictionary not enumerated → cannot decode
  }
  return PdoSampleSpec{loc->isOutput, loc->bitOffset, loc->bitLength, *dataType};
}

bool DeviceManager::deviceExchangesProcessData(uint16_t slavePosition) const {
  std::shared_lock lock(busMutex_);
  const Device* device = findDevice(slavePosition);
  return device != nullptr && device->exchangesProcessData();
}

std::expected<DeviceParameterValue, std::string> DeviceManager::readDeviceParameter(
    uint16_t slavePosition, uint16_t index, uint8_t subindex) {
  // Shared lock: this is the entry point monitoring calls from its own threads, so it must be
  // serialised against the exclusive mutators (init/scan/reset/…) that rebuild devices_/driver_.
  std::shared_lock lock(busMutex_);
  Device* device = findDevice(slavePosition);
  if (!device) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  // Routing (live PDO image when exchanging + healthy, SDO otherwise) lives in Device, which holds
  // the process-data runtime we injected at scan(). This entry point only resolves the position and
  // takes the shared lock so an off-thread caller is serialised against a device-set rebuild.
  return device->readParameter(index, subindex);
}

std::expected<void, std::string> DeviceManager::writeDeviceParameter(uint16_t slavePosition,
                                                                     uint16_t index,
                                                                     uint8_t subindex,
                                                                     DeviceParameterValue value) {
  // Shared lock: serialise against the exclusive mutators that rebuild devices_/driver_, so a
  // write that lands here off the control-plane thread can never see a half-torn device set.
  std::shared_lock lock(busMutex_);
  Device* device = findDevice(slavePosition);
  if (!device) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  // Routing (stage into the output image when exchanging + output-mapped, SDO otherwise) lives in
  // Device. This entry point only resolves the position and takes the shared lock so an off-thread
  // caller is serialised against a device-set rebuild.
  return device->writeParameter(index, subindex, std::move(value));
}

void to_json(nlohmann::json& j, const ProcessImageObjectInfo& obj) {
  j = {{"slavePosition", obj.slavePosition}, {"index", obj.index},
       {"subindex", obj.subindex},           {"name", obj.name},
       {"bitOffset", obj.bitOffset},         {"bitLength", obj.bitLength}};
}

void to_json(nlohmann::json& j, const ProcessImageInfo& info) {
  j = {{"configured", info.configured},
       {"outputBytes", info.outputBytes},
       {"inputBytes", info.inputBytes},
       {"expectedWkc", info.expectedWkc},
       {"lastWkc", info.lastWkc},
       {"healthy", info.healthy},
       {"generations", info.generations},
       {"outputs", info.outputs},
       {"inputs", info.inputs}};
}

void to_json(nlohmann::json& j, const SlaveConfigInfo& info) {
  const auto& c = info.config;
  nlohmann::json syncManagers = nlohmann::json::array();
  std::transform(c.syncManagers.begin(), c.syncManagers.end(), std::back_inserter(syncManagers),
                 [](const mm::comm::SyncManagerConfig& sm) {
                   return nlohmann::json{{"index", sm.index},
                                         {"physicalStart", sm.physicalStart},
                                         {"length", sm.length},
                                         {"flags", sm.flags},
                                         {"type", sm.type}};
                 });
  nlohmann::json fmmus = nlohmann::json::array();
  std::transform(c.fmmus.begin(), c.fmmus.end(), std::back_inserter(fmmus),
                 [](const mm::comm::FmmuConfig& f) {
                   return nlohmann::json{{"index", f.index},
                                         {"logicalStart", f.logicalStart},
                                         {"length", f.length},
                                         {"logicalStartBit", f.logicalStartBit},
                                         {"logicalEndBit", f.logicalEndBit},
                                         {"physicalStart", f.physicalStart},
                                         {"physicalStartBit", f.physicalStartBit},
                                         {"type", f.type},
                                         {"active", f.active != 0}};
                 });
  j = {{"slavePosition", c.slavePosition},
       {"deviceName", info.deviceName},
       {"configuredAddress", c.configuredAddress},
       {"aliasAddress", c.aliasAddress},
       {"outputBits", c.outputBits},
       {"inputBits", c.inputBits},
       {"mailbox",
        {{"writeLength", c.mailbox.writeLength},
         {"writeOffset", c.mailbox.writeOffset},
         {"readLength", c.mailbox.readLength},
         {"readOffset", c.mailbox.readOffset},
         {"protocols", c.mailbox.protocols}}},
       {"dc",
        {{"capable", c.dc.capable},
         {"active", c.dc.active},
         {"propagationDelay", c.dc.propagationDelay},
         {"cycleTime", c.dc.cycleTime},
         {"shift", c.dc.shift}}},
       {"syncManagers", syncManagers},
       {"fmmus", fmmus}};
}

void to_json(nlohmann::json& j, const DeviceDiagnosticsInfo& info) {
  const auto& d = info.diagnostics;
  nlohmann::json ports = nlohmann::json::array();
  std::transform(d.ports.begin(), d.ports.end(), std::back_inserter(ports),
                 [](const mm::comm::PortDiagnostics& p) {
                   return nlohmann::json{{"linkUp", p.linkUp},
                                         {"loopClosed", p.loopClosed},
                                         {"communication", p.communication},
                                         {"invalidFrame", p.invalidFrame},
                                         {"rxError", p.rxError},
                                         {"forwardedError", p.forwardedError},
                                         {"lostLink", p.lostLink}};
                 });
  j = {{"slavePosition", d.slavePosition},
       {"deviceName", info.deviceName},
       {"ports", ports},
       {"processingUnitError", d.processingUnitError},
       {"pdiError", d.pdiError},
       {"processDataWatchdog", d.processDataWatchdog},
       {"pdiWatchdog", d.pdiWatchdog}};
}

void to_json(nlohmann::json& j, const DcSyncInfo& info) {
  const auto& d = info.dcSync;
  j = {{"slavePosition", d.slavePosition},
       {"deviceName", info.deviceName},
       {"dcCapable", d.dcCapable},
       {"referenceClock", d.referenceClock},
       {"propagationDelay", d.propagationDelay},
       {"systemTimeDifference", d.systemTimeDifference}};
}

void to_json(nlohmann::json& j, const DeviceManager& dm) { j = dm.devices(); }

}  // namespace mm::node
