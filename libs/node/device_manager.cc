#include "node/device_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/platform.h"  // userCacheDir — the default root for recorder dumps
#include "node/process_data.h"
#include "node/process_data_dump.h"

namespace mm::node {

// ProcessData (the live process-data runtime: published image pointer + recorder ring +
// working-counter health) is defined in node/process_data.h so device.cc can use it too — a Device
// reaches its live IOmap objects through ProcessData::readPdo / writePdo. Those two accessors are
// defined out-of-line below, after the bit/byte helpers they use. Published images are retained in
// ProcessData::generations until reset() so a lock-free reader never dereferences a freed image.

DeviceManager::DeviceManager()
    : currentSet_(std::make_shared<DeviceSet>()), pd_(std::make_unique<ProcessData>()) {
  // An empty set from construction on, so deviceSet() never returns null and no reader needs a
  // "before init()" branch. The RT view stays null until something is published.
}

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

// The EtherCAT AL state machine, keyed by a device's current state: each entry lists every state
// that state may transition to (including itself — re-commanding the current state is harmless).
// Climbs are single-step only (INIT -> PRE-OP -> SAFE-OP -> OP); drops may skip levels (e.g.
// OP -> INIT); BOOT is reachable only from INIT and only returns to INIT.
//
// Motion Master must reject illegal transitions itself rather than leaving them to the slave (which
// answers an illegal request with AL status 0x0011): entering SAFE-OP/OP triggers a re-map that
// reads each device's PDO mapping over the CoE mailbox, which is only live from PRE-OP up. A device
// asked to jump straight from BOOT (firmware-sized mailbox, no CoE) into an exchange state would
// reach that mailbox read while still in BOOT and segfault inside SOEM before the slave ever sees
// the request. Validating here keeps the bad jump away from the mapper entirely.
using mm::comm::EtherCatState;
// The only throw this static initialisation can produce is bad_alloc, allocating a five-entry
// table before main runs; a process that cannot afford that has nothing to fall back to. The map
// is what makes the rule above readable, and a constexpr array would trade that for nothing.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const std::map<EtherCatState, std::set<EtherCatState>> kValidStateTransitions = {
    {EtherCatState::Init, {EtherCatState::Init, EtherCatState::PreOp, EtherCatState::Boot}},
    {EtherCatState::PreOp, {EtherCatState::PreOp, EtherCatState::Init, EtherCatState::SafeOp}},
    {EtherCatState::SafeOp,
     {EtherCatState::SafeOp, EtherCatState::PreOp, EtherCatState::Init, EtherCatState::Op}},
    {EtherCatState::Op,
     {EtherCatState::Op, EtherCatState::SafeOp, EtherCatState::PreOp, EtherCatState::Init}},
    {EtherCatState::Boot, {EtherCatState::Boot, EtherCatState::Init}},
};

// Whether the EtherCAT AL state machine permits a direct transition from currentState to
// targetState. An unrecognised current state (e.g. a lost device reporting 0) permits nothing.
bool isValidStateTransition(EtherCatState currentState, EtherCatState targetState) {
  const auto allowed = kValidStateTransitions.find(currentState);
  return allowed != kValidStateTransitions.end() && allowed->second.contains(targetState);
}

}  // namespace

std::expected<void, std::string> DeviceManager::init(
    std::unique_ptr<mm::comm::FieldbusDriver> driver, const DeviceManagerConfig& config) {
  // One control-plane operation at a time. There is no second lock to take: publishing a set is a
  // pointer swap, and no reader can observe a driver whose context is not yet open, because the set
  // that carries it is published only after init() succeeds.
  const std::lock_guard busOperationLock(busOperationMutex_);
  // init() is a one-shot. A live driver stays until reset(), because the devices of the current set
  // talk through it and a caller may still hold that set.
  if (deviceSet()->driver) {
    return std::unexpected("already initialised — call reset() before init()");
  }
  // Retain the config for configureProcessData, which allocates the recorder ring (recorderCapacity
  // cycles) once the image — and hence the per-record byte size — is known.
  config_ = config;
  std::shared_ptr<mm::comm::FieldbusDriver> shared(std::move(driver));
  auto result = shared->init();
  if (!result) {
    // A failed init must leave us uninitialised — not holding a driver whose context never opened.
    // Otherwise initialised() would report true and the next scan()/SDO call would dereference a
    // null context. Publishing nothing here also lets the caller simply retry init() without an
    // intervening reset().
    spdlog::error("FieldbusDriver init failed: {}", result.error());
    return result;
  }
  // The driver is live: publish a set that owns it and holds no devices yet. scan() replaces it.
  auto set = std::make_shared<DeviceSet>();
  set->driver = std::move(shared);
  set->topologyGeneration = topologyGeneration_.load(std::memory_order_relaxed);
  // Nothing to drain on a first init, and nothing here frees anything the RT thread can reach, so
  // the outcome is not load-bearing.
  static_cast<void>(stopExchange());
  publishDeviceSet(std::move(set));
  spdlog::debug("FieldbusDriver initialised");
  return result;
}

void DeviceManager::configureParameterCache(const ParameterCacheConfig& config) {
  parameterCache_.configure(config);
}

const ParameterCache& DeviceManager::parameterCache() const { return parameterCache_; }

std::expected<int, std::string> DeviceManager::scan() {
  // busOperationMutex_ only. The driver scan reprograms the SOEM context underneath the *current*
  // set, so this must exclude the other control-plane operations — but not readers: they keep
  // working against the set they already hold, and this publishes a new one when it is complete.
  const std::lock_guard busOperationLock(busOperationMutex_);
  const std::shared_ptr<DeviceSet> previous = deviceSet();
  if (!previous->driver) {
    spdlog::error("scan() called with no driver — call init() first");
    return std::unexpected("no driver — call init() first");
  }
  // A re-scan reprograms the whole SOEM context (ecx_config_init rebuilds the slavelist, sync
  // managers, and FMMUs) and publishes a new device set, invalidating the current process image.
  // Unpublish and drain the RT cycle first — exactly as reset()/configureProcessData() do — so
  // exchangeProcessData() is a no-op and the RT loop is no longer touching the IOmap while the
  // driver rebuilds it. Then reclaim the now-stale image generations (safe once exchange is
  // gated off); a fresh image is published when the bus is next brought into SAFE-OP/OP.
  if (!stopExchange()) {
    // Refused rather than forced. A scan frees the recording, the retained images and — once this
    // set is replaced — every device and cell in it, and the RT thread is demonstrably still
    // reading them. Nothing is touched yet, so the bus is exactly as it was and the caller
    // may retry.
    return std::unexpected(
        "the real-time loop did not leave its cycle within 200 ms, so nothing was changed — check "
        "GET /api/game-loop for a stalled loop or a cyclic task that overruns");
  }
  {
    // The recording and the retained images describe a device set that is about to be replaced, so
    // both are discarded. Exclusive, because readers hold this shared while they walk either one.
    const std::unique_lock processDataLock(processDataMutex_);
    pd_->generations.clear();
    pd_->ring.clear();
  }
  auto result = previous->driver->scan();
  if (!result) {
    spdlog::error("FieldbusDriver scan failed: {}", result.error());
    return std::unexpected(result.error());
  }
  // Positions may now name different devices, so this is a new generation.
  const uint64_t generation = topologyGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
  auto set = std::make_shared<DeviceSet>();
  set->driver = previous->driver;  // same driver, new devices
  set->topologyGeneration = generation;
  set->devices.reserve(static_cast<size_t>(*result));
  for (uint16_t pos = 1; pos <= static_cast<uint16_t>(*result); ++pos) {
    // Hand each device the process-data runtime so its read/writeParameter can serve the live
    // IOmap value while exchanging (and stage outputs), falling back to SDO otherwise, plus the
    // shared parameter cache so initializeParameters can skip enumeration on a hit. Both pd_ and
    // parameterCache_ are members created once and never replaced, so the pointers are stable for
    // our lifetime.
    set->devices.emplace_back(pos, *set->driver, pd_.get(), &parameterCache_);
  }
  spdlog::info("Found {} slave(s)", *result);
  for (const auto& found : set->devices) {
    spdlog::info("  [{:2}] {} — vendor: {:#010x}  product: {:#010x}  rev: {:#010x}  serial: {}",
                 found.slavePosition(), found.name(), found.vendorId(), found.productCode(),
                 found.revisionNumber(), found.serialNumber());
  }
  publishDeviceSet(std::move(set));
  return *result;
}

void DeviceManager::reset() {
  const std::lock_guard busOperationLock(busOperationMutex_);
  const std::shared_ptr<DeviceSet> previous = deviceSet();
  // Unpublish the image and drain any in-flight cycle so a concurrent exchangeProcessData
  // becomes a no-op, then reclaim every retained image generation (safe now that exchange is
  // gated off).
  // reset() never refuses. It is the operation an operator reaches for *because* something is
  // wrong, so a stalled cycle must not block it. It holds the memory back instead: the retired set
  // and the retained images move to abandoned_, the ring keeps its storage, and the next successful
  // drain reclaims all of it.
  const bool drained = stopExchange();
  // The end of the session the short-working-counter record describes: the driver is about to go,
  // and a later init() starts a new bus. The only place these are cleared.
  pd_->shortWkcCycles.store(0, std::memory_order_relaxed);
  pd_->firstShortWkcNs.store(0, std::memory_order_relaxed);
  pd_->lastShortWkcNs.store(0, std::memory_order_relaxed);
  {
    const std::unique_lock processDataLock(processDataMutex_);
    if (drained) {
      pd_->generations.clear();
      pd_->ring.clear();  // teardown — free the recorder storage
    } else {
      // Moved rather than copied out: abandoned_ takes over every reference, and clearing then
      // drops only ours.
      abandoned_.insert(abandoned_.end(), pd_->generations.begin(), pd_->generations.end());
      pd_->generations.clear();
    }
  }
  if (!drained) {
    // The set is about to be replaced, and dropping the last reference to it would free the devices
    // and cells the RT thread is still reading.
    abandoned_.push_back(previous);
  }
  topologyGeneration_.fetch_add(1, std::memory_order_relaxed);
  // An empty set takes over, so every reader gets a set with no devices and no driver from here on.
  // Publishing it is what drops this manager's reference to the old one. Whoever still holds a
  // DeviceHandle keeps their own copy alive until they are done with it, and the drain above means
  // no cyclic task can be inside a cycle holding a pointer into it.
  auto set = std::make_shared<DeviceSet>();
  set->topologyGeneration = topologyGeneration_.load(std::memory_order_relaxed);
  publishDeviceSet(std::move(set));
  if (previous->driver) {
    // Stop the bus now rather than when the last holder of the retired set drops it: a procedure
    // still running against a retired device must fail its next transfer, not keep driving
    // hardware the user asked us to release. The driver object itself lives until that holder
    // is done with it, which is what keeps the failure a clean error instead of a crash.
    previous->driver->stop();
    spdlog::info("DeviceManager reset");
  }
}

uint64_t DeviceManager::topologyGeneration() const {
  return topologyGeneration_.load(std::memory_order_relaxed);
}

bool DeviceManager::initialised() const { return deviceSet()->driver != nullptr; }

bool DeviceManager::hasDevice(uint16_t slavePosition) const {
  return deviceSet()->find(slavePosition) != nullptr;
}

DeviceHandle DeviceManager::deviceAt(uint16_t slavePosition) const {
  std::shared_ptr<DeviceSet> set = deviceSet();
  Device* found = set->find(slavePosition);
  return DeviceHandle(std::move(set), found);
}

std::shared_ptr<DeviceSet> DeviceManager::deviceSet() const {
  const std::lock_guard lock(currentSetMutex_);
  return currentSet_;
}

void DeviceManager::publishDeviceSet(std::shared_ptr<DeviceSet> set) {
  // The RT view first, while the argument still keeps the new set alive. Assigning currentSet_ is
  // what drops the previous set, so doing that second means publishedSet_ never names an object
  // that nothing owns.
  publishedSet_.store(set.get(), std::memory_order_release);
  const std::lock_guard lock(currentSetMutex_);
  currentSet_ = std::move(set);
}

Device* DeviceSet::find(uint16_t slavePosition) {
  auto it = std::find_if(devices.begin(), devices.end(), [slavePosition](const Device& d) {
    return d.slavePosition() == slavePosition;
  });
  return it != devices.end() ? &*it : nullptr;
}

const Device* DeviceSet::find(uint16_t slavePosition) const {
  auto it = std::find_if(devices.begin(), devices.end(), [slavePosition](const Device& d) {
    return d.slavePosition() == slavePosition;
  });
  return it != devices.end() ? &*it : nullptr;
}

const Device* DeviceManager::findDevice(uint16_t slavePosition) const {
  const DeviceSet* set = publishedSet_.load(std::memory_order_acquire);
  return set != nullptr ? set->find(slavePosition) : nullptr;
}

Device* DeviceManager::findDevice(uint16_t slavePosition) {
  DeviceSet* set = publishedSet_.load(std::memory_order_acquire);
  return set != nullptr ? set->find(slavePosition) : nullptr;
}

std::expected<void, std::string> DeviceManager::configureProcessData() {
  // busOperationMutex_ only: it keeps the published set from changing underneath the re-map and
  // serialises against the other control-plane operations. The bus I/O and the per-device
  // PDO-mapping reads run without excluding a single reader.
  const std::lock_guard busOperationLock(busOperationMutex_);
  return remapProcessImage();
}

std::expected<void, std::string> DeviceManager::remapProcessImage() {
  // The caller holds busOperationMutex_, so this set stays the published one for the whole re-map:
  // only a holder of that lock publishes another.
  const std::shared_ptr<DeviceSet> set = deviceSet();
  if (!set->driver) {
    return std::unexpected("configureProcessData: no driver — call init() first");
  }
  if (set->devices.empty()) {
    return std::unexpected("configureProcessData: no devices — call scan() first");
  }
  // Unpublish first and drain any in-flight cycle so the RT thread is not touching the IOmap
  // while we re-map it. Refused rather than forced: this re-allocates the ring and has the driver
  // rewrite the IOmap, both of which the RT thread is still reading if the drain failed. Nothing
  // changes at this point.
  if (!stopExchange()) {
    return std::unexpected(
        "the real-time loop did not leave its cycle within 200 ms, so the process image was not "
        "re-mapped — check GET /api/game-loop for a stalled loop or a cyclic task that overruns");
  }

  if (auto r = set->driver->configureProcessData(); !r) {
    return std::unexpected(r.error());
  }
  for (auto& device : set->devices) {
    if (auto r = device.readFlatPdoMapping(); !r) {
      return std::unexpected(r.error());
    }
  }
  auto image = buildProcessImage(set->driver->processDataLayout(), set->devices);
  if (!image) {
    return std::unexpected(image.error());
  }

  // Nothing to seed: each output object's value already lives in its own parameter's cell, which is
  // what the composer reads. The RxPDO setpoints callers initialised before OP (controlword, modes,
  // targets) are therefore sent on the very first cycle by construction, and a write that lands
  // while this re-map is running is picked up rather than lost between a seed and a publish.

  // The publish window. Only the recorder's storage needs a lock here, because it is the one thing
  // a reader can be walking while this frees it; the image pointer is published atomically and its
  // predecessors are retained. Everything above (the IOmap rebuild, the per-device PDO-mapping SDO
  // reads and building the image) ran under busOperationMutex_ alone, so the sampler kept flushing
  // throughout.
  std::shared_ptr<const ProcessImage> shared;
  {
    const std::unique_lock recorderLock(processDataMutex_);
    // (Re)allocate the recorder for this image's per-cycle byte size and the configured depth. A
    // layout-changing re-map restarts the recording — records under the old layout are undecodable
    // under the new one. Exchange is drained (stopExchange above) and the image is not yet
    // published, so the RT writer cannot touch the ring while it is being rebuilt.
    pd_->ring.allocate(image->inputBytes, image->outputBytes, config_.recorderCapacity);

    shared = std::make_shared<const ProcessImage>(std::move(*image));
    pd_->generations.push_back(shared);
    pd_->image.store(shared.get(), std::memory_order_release);
    // A new image is published: object offsets may differ from the previous one, so signal
    // consumers that captured the layout (the monitoring sampler) to re-capture it.
    processImageGeneration_.fetch_add(1, std::memory_order_relaxed);
    // The short-working-counter counters are deliberately *not* cleared here. A re-map happens
    // whenever anyone brings a device into or out of SAFE-OP/OP, which is precisely when someone is
    // chasing a fault — so clearing per generation erased the history at the moment it was most
    // wanted, and left the count describing a window nobody chose. They run from init() to reset()
    // instead, and `generations` already says how many images that spans.
    updateExpectedWkc();
  }
  spdlog::info("Process data configured: {} output bytes, {} input bytes, expected WKC {}",
               shared->outputBytes, shared->inputBytes, shared->expectedWkc);
  return {};
}

void DeviceManager::exchangeProcessData() {
  // The published image is the *only* gate, and deliberately so. The driver lives in the published
  // device set, and an image can only be published by remapProcessImage(), which requires one; a
  // reset() unpublishes the image and drains this cycle before it publishes an empty set. So a
  // non-null image below means a live set with a live driver, established by the ordering rather
  // than by a check the RT thread has no safe way to make.
  //
  // Raise the in-flight flag BEFORE reading the published image, then re-read the image: this
  // closes the race against stopExchange(), which stores nullptr and then waits on this flag.
  // Both the flag store and the image load are sequentially consistent so they cannot be
  // reordered against stopExchange()'s image store / flag load (a StoreLoad pair that only
  // seq_cst prevents). The total order then guarantees that for any concurrent teardown either
  // we observe the null image and back out here, or stopExchange() observes the flag and drains
  // us — never both missing each other and letting us touch a half-remapped IOmap.
  pd_->inCycle.fetch_add(1, std::memory_order_seq_cst);
  const ProcessImage* image = pd_->image.load(std::memory_order_seq_cst);
  if (image == nullptr) {
    pd_->inCycle.fetch_sub(1, std::memory_order_release);
    return;  // not mapped yet, or torn down mid-flight — back out without touching the IOmap
  }
  // Compose the output image from each output object's parameter cell — this RT thread is the sole
  // writer of outScratch, so no lock is needed and bit-packed objects sharing a byte are applied
  // safely in sequence. Zero the live region first so alignment gaps (and any object with no
  // parameter behind it) go out as zero.
  const uint32_t outputBytes = image->outputBytes;
  std::fill_n(pd_->outScratch.bytes.begin(), outputBytes, uint8_t{0});
  pd_->outScratch.size = outputBytes;
  const std::span<uint8_t> outputs(pd_->outScratch.bytes.data(), outputBytes);
  std::array<uint8_t, sizeof(uint64_t)> buf{};
  for (const ProcessImageEntry& entry : image->outputs) {
    if (entry.parameter == nullptr) {
      continue;  // dictionary not enumerated — nothing to send, the zeroed region stands
    }
    // Unpack into a stack buffer — no allocation, no lookup on the RT path.
    unpackLeBits(entry.parameter->loadBits(), buf);
    insertBits(outputs, entry.bitOffset, entry.bitLength, buf);
  }
  // Exchange one cycle, then append it to the recorder for non-RT readers. The scratch buffers are
  // touched only on this (RT) thread; ring.write() copies just the live image bytes (the published
  // sizes, which are stable for this generation) into the next ring slot, keeping per-cycle memory
  // traffic proportional to the real image.
  // The set the image was built from: the control plane replaces this pointer only with the cycle
  // drained, and we are inside the cycle.
  DeviceSet* set = publishedSet_.load(std::memory_order_acquire);
  const int wkc = set->driver->exchangeProcessData(
      std::span<const uint8_t>(pd_->outScratch.bytes.data(), outputBytes),
      std::span<uint8_t>(pd_->inScratch.bytes.data(), image->inputBytes));
  pd_->inScratch.size = image->inputBytes;
  // Publish the working counter for health checks; expectedWkc is maintained off the RT path.
  pd_->lastWkc.store(wkc, std::memory_order_relaxed);
  // Record this cycle: one lock-free append carrying both directions from the same exchange, the
  // single source for the live monitoring stream and freshest-value point reads. On a lost or
  // partial frame the driver leaves the prior bytes in the IOmap so the recorded inputs may be
  // stale — readers gate on the working counter (processDataHealthy()), not here. The timestamp is
  // wall-clock epoch nanoseconds (reduced to microseconds at the JSON boundary; see
  // ProcessDataRing).
  const uint64_t timestampNs =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
  // Note a cycle the bus did not fully answer, with when it happened. At most three stores, on a
  // path that just blocked on a frame round trip — and the only place the fault is visible, since
  // no other thread sees every cycle.
  //
  // The counter is written last, with release, so that it is the publish fence for the two
  // timestamps: a reader that acquires the new count is guaranteed to see the timestamps that
  // belong to it, rather than a fresh count beside a stale or still-zero time. Reading the counter
  // first rather than using fetch_add is what allows that ordering, and is exact because this
  // thread is its only writer while the bus exchanges (reset() clears it with the cycle drained).
  if (wkc < pd_->expectedWkc.load(std::memory_order_relaxed)) {
    const uint64_t shortCycles = pd_->shortWkcCycles.load(std::memory_order_relaxed);
    if (shortCycles == 0) {
      pd_->firstShortWkcNs.store(timestampNs, std::memory_order_relaxed);
    }
    pd_->lastShortWkcNs.store(timestampNs, std::memory_order_relaxed);
    pd_->shortWkcCycles.store(shortCycles + 1, std::memory_order_release);
  }
  pd_->ring.write(timestampNs, wkc,
                  std::span<const uint8_t>(pd_->inScratch.bytes.data(), image->inputBytes),
                  std::span<const uint8_t>(pd_->outScratch.bytes.data(), outputBytes));
  decodeInputsIntoCells(*image);
  pd_->inCycle.fetch_sub(1, std::memory_order_release);
}

void DeviceManager::decodeInputsIntoCells(const ProcessImage& image) {
  // Every mapped input lands in its parameter's cell, every cycle, read or not. That is what makes
  // Device::value<T>() a single atomic load rather than an image lookup plus a bit extraction, and
  // it is what a cyclic task's whole read path rests on.
  //
  // Deliberately ungated on the working counter. A short WKC means the driver left the previous
  // cycle's bytes in the IOmap, so the cells hold the last value that did arrive — which is what
  // "last known value" means and what a control loop can act on. The alternative, diverting to a
  // blocking SDO upload, is not available on this thread at all. The WKC is recorded with the cycle
  // in the ring, so a caller that needs to know reads it there (processDataHealthy()).
  //
  // Non-allocating and lookup-free: one stack buffer reused for every object, and the owning
  // parameter resolved when the image was built.
  const std::span<const uint8_t> inputs(pd_->inScratch.bytes.data(), image.inputBytes);
  std::array<uint8_t, sizeof(uint64_t)> buf{};
  for (const ProcessImageEntry& entry : image.inputs) {
    if (entry.parameter == nullptr) {
      continue;  // dictionary not enumerated — the object exchanges, it just has no cell
    }
    extractBits(inputs, entry.bitOffset, entry.bitLength, buf);
    entry.parameter->storeBits(packLeBits(buf));
  }
}

DeviceManager::CycleGuard::CycleGuard(DeviceManager& deviceManager)
    : processData_(deviceManager.pd_.get()), entered_(false) {
  // The same handshake exchangeProcessData uses, one level up so it covers the task's own device
  // and parameter lookups: raise the depth BEFORE loading the image, then load it. Both are
  // sequentially consistent so they cannot be reordered against stopExchange()'s image-store /
  // depth-load (a StoreLoad pair only seq_cst prevents). The resulting total order guarantees that
  // for any concurrent rebuild either we observe the null image and take nothing, or stopExchange
  // observes our depth and waits us out — never both missing each other.
  processData_->inCycle.fetch_add(1, std::memory_order_seq_cst);
  if (processData_->image.load(std::memory_order_seq_cst) == nullptr) {
    processData_->inCycle.fetch_sub(1, std::memory_order_release);
    return;
  }
  entered_ = true;
}

DeviceManager::CycleGuard::~CycleGuard() {
  if (entered_) {
    processData_->inCycle.fetch_sub(1, std::memory_order_release);
  }
}

bool DeviceManager::stopExchange() {
  const ProcessData::PauseResult paused = pd_->pauseCycle();
  if (!paused.drained) {
    return false;
  }
  // The drain succeeded, so the RT thread is out of the cycle and anything an earlier failed drain
  // left alive is now unreachable by it. This is the only reclaim point for that backlog, and it is
  // empty unless a drain failed before.
  if (!abandoned_.empty()) {
    spdlog::info("Reclaiming {} object(s) held back by an earlier stalled cycle",
                 abandoned_.size());
    abandoned_.clear();
  }
  return true;
}

ProcessData::PauseResult ProcessData::pauseCycle() {
  const ProcessImage* previous = image.exchange(nullptr, std::memory_order_seq_cst);
  // Drain whatever the RT thread has in flight — the exchange itself, and any cyclic task body
  // holding a CycleGuard (which resolves devices and parameters of its own, so it must be out
  // before scan/reset destroy them). Both operations here are sequentially consistent so they pair
  // with each raiser's seq_cst depth-increment / image-load: once we store the null image,
  // any RT cycle that already raised the depth is visible to this load, and any that did not
  // yet raised it will observe the null image and back out. We therefore only wait out the
  // at-most-one cycle already in flight. Bounded so a stalled/absent RT loop can never hang a
  // control-plane call.
  // Sleep rather than spin. A yield loop burns a core for the whole wait, and if the RT thread is
  // not pinned to one of its own it can delay the very thread being waited for. 50 µs is short
  // against the 200 ms bound and long enough to cost nothing.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (inCycle.load(std::memory_order_seq_cst) != 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  // Expiry is reported, never papered over. The RT thread is still inside a cycle, so everything
  // this caller was about to free — the ring storage, the IOmap, a device set, a parameter map — is
  // still being read. A caller that sees drained == false must free nothing: it either refuses the
  // operation or keeps the object alive for a later reclaim. An RT thread preempted for a fifth of
  // a second is itself the diagnosis worth reporting.
  if (inCycle.load(std::memory_order_seq_cst) != 0) {
    spdlog::warn(
        "RT cycle did not drain within 200 ms. The RT loop is stalled or descheduled; nothing will "
        "be freed while that is true.");
    return PauseResult{.previous = previous, .drained = false};
  }
  return PauseResult{.previous = previous, .drained = true};
}

void ProcessData::resumeCycle(const ProcessImage* previous) {
  image.store(previous, std::memory_order_release);
}

// The three recorder accessors take processDataMutex_ shared, and the adversary is not the RT
// producer. ProcessDataRing's lock-free protocol makes a reader safe against write() — a concurrent
// append, even one that laps the reader, is detected by the per-slot sequence re-check. It says
// nothing about allocate()/clear(), which *release the storage* (buffer_ and the seqWords_ vector)
// and run from the control plane: a re-map re-allocates because records under the old layout are
// undecodable, and scan()/reset() clear. Those take processDataMutex_ exclusively, so reading the
// ring without it is a use-after-free, not a torn read — and the window is wide, since a sampler
// flush walks thousands of records. Shared, so the exclusive rebuilders are the only thing that
// waits; serializeDump reads the ring under the same lock.
uint64_t DeviceManager::recorderHead() const {
  const std::shared_lock lock(processDataMutex_);
  return pd_->ring.head();
}

uint64_t DeviceManager::recorderOldestSeq() const {
  const std::shared_lock lock(processDataMutex_);
  return pd_->ring.oldestValidSeq();
}

bool DeviceManager::readRecord(uint64_t seq, ProcessDataRing::Record& out) const {
  // Per record rather than per span: the lock is held for one record's copy, which keeps the
  // sampler from becoming a milliseconds-long shared holder that an exclusive scan would queue
  // behind. A re-map part-way through a span therefore ends it — every record already reads
  // independently (a lapped one returns false and the caller resyncs), so a span was never atomic.
  const std::shared_lock lock(processDataMutex_);
  return pd_->ring.readRecord(seq, out);
}

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

uint64_t DeviceManager::shortWkcCycles() const {
  return pd_->shortWkcCycles.load(std::memory_order_acquire);
}

ProcessImageInfo DeviceManager::processImageInfo() const {
  // Shared: generations is a vector the control plane appends to (a re-map) and clears
  // (scan/reset), so walking it and dereferencing its back() needs the lock those writers take.
  const std::shared_lock lock(processDataMutex_);
  const std::shared_ptr<DeviceSet> set = deviceSet();
  ProcessImageInfo info{};
  info.lastWkc = pd_->lastWkc.load(std::memory_order_relaxed);
  info.expectedWkc = pd_->expectedWkc.load(std::memory_order_relaxed);
  info.generations = pd_->generations.size();
  // Acquire, then the timestamps relaxed behind it: the RT writer releases this counter after
  // writing both, so this order is what makes the three consistent with each other.
  info.shortWkcCycles = pd_->shortWkcCycles.load(std::memory_order_acquire);
  // Reduced to microseconds at the JSON boundary, matching every other timestamp this API serves.
  info.firstShortWkcUs = pd_->firstShortWkcNs.load(std::memory_order_relaxed) / 1000;
  info.lastShortWkcUs = pd_->lastShortWkcNs.load(std::memory_order_relaxed) / 1000;

  const ProcessImage* image = pd_->image.load(std::memory_order_acquire);
  info.configured = image != nullptr;
  info.healthy = info.configured && info.lastWkc >= info.expectedWkc;

  // Describe the live image when exchanging; otherwise fall back to the most recent retained
  // generation so a bus that dropped out of SAFE-OP/OP (image torn down, but generations
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
  // dictionary is not enumerated — PDO mappings are read independently of OD enumeration).
  auto flatten = [&set](const std::vector<ProcessImageEntry>& entries) {
    std::vector<ProcessImageObjectInfo> out;
    out.reserve(entries.size());
    for (const auto& e : entries) {
      std::string name;
      if (const Device* device = set->find(e.slavePosition)) {
        if (auto p = device->parameter(e.index, e.subindex)) {
          name = std::move(p->name);
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

std::expected<DeviceManager::DumpSpan, std::string> DeviceManager::serializeDump(
    std::ostream& out) {
  // Shared lock: serialise against the control-plane writers of the recorder storage and the
  // retained generations (a re-map re-allocates, scan/reset clear). The RT producer is never
  // blocked — it appends to the ring lock-free; we only read the ring. The device set comes from a
  // snapshot, so a concurrent scan changes nothing here.
  std::shared_lock lock(processDataMutex_);
  const std::shared_ptr<DeviceSet> set = deviceSet();

  // Header image: the live published image, or — once the bus leaves the exchange states and the
  // image was torn down — the most recent retained generation (kept until reset()/scan()). Records
  // currently in the ring were all written under this layout (a layout-changing re-map resets the
  // ring), so it is the correct decode header for the whole span.
  const ProcessImage* image = pd_->image.load(std::memory_order_acquire);
  if (image == nullptr && !pd_->generations.empty()) {
    image = pd_->generations.back().get();
  }
  if (image == nullptr) {
    return std::unexpected("no process image has been mapped — nothing to dump");
  }
  if (!pd_->ring.allocated() || pd_->ring.head() == 0) {
    return std::unexpected("the recorder is empty — no cycles have been recorded yet");
  }

  // Freeze the span at this instant. While exchanging, the producer keeps advancing head() during
  // the write; we only serialise this [start, end) snapshot and ignore later cycles. A record the
  // producer laps mid-read is skipped by readRecord and self-describes via its sequence.
  const uint64_t startSeq = pd_->ring.oldestValidSeq();
  const uint64_t endSeq = pd_->ring.head();

  // Build the embedded header: every discovered device's identity, then its PDO objects (both
  // directions) resolved to name + data type from its parameter map (empty/0 if the OD was not
  // enumerated), in image order.
  DumpHeader header;
  header.inputBytes = image->inputBytes;
  header.outputBytes = image->outputBytes;

  std::map<uint16_t, size_t> deviceIndex;
  for (const Device& d : set->devices) {
    deviceIndex.emplace(d.slavePosition(), header.devices.size());
    header.devices.push_back(DumpDevice{.slavePosition = d.slavePosition(),
                                        .vendorId = d.vendorId(),
                                        .productCode = d.productCode(),
                                        .revisionNumber = d.revisionNumber(),
                                        .serialNumber = d.serialNumber(),
                                        .name = d.name(),
                                        .entries = {}});
  }
  auto appendEntries = [&](const std::vector<ProcessImageEntry>& entries, bool isOutput) {
    for (const ProcessImageEntry& e : entries) {
      auto it = deviceIndex.find(e.slavePosition);
      if (it == deviceIndex.end()) {
        continue;  // entry for a device the snapshot does not hold — skip
      }
      DumpPdoEntry pe{.index = e.index,
                      .subindex = e.subindex,
                      .isOutput = isOutput,
                      .dataType = 0,
                      .bitLength = e.bitLength,
                      .bitOffset = e.bitOffset,
                      .name = {}};
      if (const Device* dev = set->find(e.slavePosition)) {
        if (auto p = dev->parameter(e.index, e.subindex)) {
          pe.name = std::move(p->name);
          pe.dataType = p->dataType;
        }
      }
      header.devices[it->second].entries.push_back(std::move(pe));
    }
  };
  appendEntries(image->outputs, /*isOutput=*/true);
  appendEntries(image->inputs, /*isOutput=*/false);

  auto written = writeProcessDataDump(out, header, startSeq, endSeq,
                                      [this](uint64_t seq, ProcessDataRing::Record& rec) {
                                        return pd_->ring.readRecord(seq, rec);
                                      });
  if (!written) {
    return std::unexpected(written.error());
  }
  return DumpSpan{.rows = *written, .startSeq = startSeq, .endSeq = endSeq};
}

std::expected<std::string, std::string> DeviceManager::dumpProcessData() {
  // Resolve the output directory and create it. An empty setting means a "dumps" subdirectory of
  // the per-user cache — deliberately *not* the OS temp directory, which the platform reaps on a
  // timer (systemd-tmpfiles clears /tmp after days), silently deleting dumps a user meant to keep.
  // Landing under the cache root also puts them in reach of the /api/user-cache endpoints, so a
  // dump can be listed, downloaded and deleted remotely instead of only by logging into the host.
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path dir = config_.recorderDumpDir.empty() ? mm::core::userCacheDir() / "dumps"
                                                 : fs::path(config_.recorderDumpDir);
  fs::create_directories(dir, ec);
  if (ec) {
    return std::unexpected("could not create dump directory '" + dir.string() +
                           "': " + ec.message());
  }

  // Stream into a unique temp file, then rename to dump-<UTC>-<endSeq>.mmpd once the span is known
  // (the trailing sequence is only final after serialisation). Direct-to-file streaming avoids
  // buffering the whole span in memory.
  static std::atomic<uint64_t> tmpCounter{0};
  const fs::path tmp =
      dir / std::format("dump-{}.partial", tmpCounter.fetch_add(1, std::memory_order_relaxed));
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out) {
    return std::unexpected("could not open dump file '" + tmp.string() + "' for writing");
  }

  auto span = serializeDump(out);
  out.close();
  if (!span) {
    fs::remove(tmp, ec);  // do not leave a truncated file behind
    return std::unexpected(span.error());
  }

  const auto nowSec = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  const fs::path path = dir / std::format("dump-{:%Y%m%dT%H%M%SZ}-{}.mmpd", nowSec, span->endSeq);
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return std::unexpected("could not finalise dump file '" + path.string() + "': " + ec.message());
  }
  spdlog::info("Dumped {} process-data cycles ([{}, {})) to {}", span->rows, span->startSeq,
               span->endSeq, path.string());
  return path.string();
}

std::expected<std::string, std::string> DeviceManager::dumpProcessDataBuffer() {
  std::ostringstream out(std::ios::binary);
  auto span = serializeDump(out);
  if (!span) {
    return std::unexpected(span.error());
  }
  spdlog::info("Serialised {} process-data cycles ([{}, {})) for streaming", span->rows,
               span->startSeq, span->endSeq);
  return std::move(out).str();
}

std::vector<SlaveConfigInfo> DeviceManager::busConfig() const {
  // Shared, like every other position-based read surface: this dereferences set->driver and
  // resolves slave positions against set->devices, both of which the exclusive rebuilders replace.
  const std::shared_ptr<DeviceSet> set = deviceSet();
  std::vector<SlaveConfigInfo> out;
  if (!set->driver) {
    return out;
  }
  auto configs = set->driver->busConfig();
  out.reserve(configs.size());
  for (auto& c : configs) {
    SlaveConfigInfo info{};
    info.config = std::move(c);
    if (const Device* device = set->find(info.config.slavePosition)) {
      info.deviceName = device->name();
      info.productName = device->productName();
      info.vendorId = device->vendorId();
      info.productCode = device->productCode();
      info.revisionNumber = device->revisionNumber();
      info.serialNumber = device->serialNumber();
    }
    out.push_back(std::move(info));
  }
  return out;
}

std::expected<std::vector<uint16_t>, std::string> DeviceManager::resolveTargets(
    const std::vector<uint16_t>& positions) const {
  const std::shared_ptr<DeviceSet> set = deviceSet();
  if (positions.empty()) {
    std::vector<uint16_t> all;
    all.reserve(set->devices.size());
    std::transform(set->devices.begin(), set->devices.end(), std::back_inserter(all),
                   [](const Device& d) { return d.slavePosition(); });
    return all;
  }
  // Validate every caller-supplied position against the device set before it reaches the
  // driver: the slave-indexed driver accessors (slaveState, readStates, readDiagnostics,
  // readDcSync, transitionToState) index a fixed-size slavelist without bounds-checking, so an
  // unknown/out-of-range position would be an out-of-bounds read (and, for a state change, a
  // write to a bogus station). Reject it here, mirroring the 404 the single-device routes give.
  auto unknown =
      std::ranges::find_if(positions, [&set](uint16_t pos) { return set->find(pos) == nullptr; });
  if (unknown != positions.end()) {
    return std::unexpected("unknown device position " + std::to_string(*unknown));
  }
  return positions;
}

int DeviceManager::expectedWkcDuring(std::span<const uint16_t> transitioning,
                                     std::optional<mm::comm::EtherCatState> target) const {
  const std::shared_ptr<DeviceSet> set = deviceSet();
  // Sum each non-errored device's working-counter contribution for its AL state and PDO presence.
  // The protocol rule (how outputs/inputs and SAFE-OP/OP map to a WKC increment) lives in the comm
  // layer; here we only know each device's state and whether it maps any PDO, so the figure tracks
  // a partially-operational bus — what a health check compares against.
  int expected = 0;
  if (!set->driver) {
    return expected;
  }
  for (const auto& device : set->devices) {
    const uint16_t status = set->driver->slaveState(device.slavePosition());
    if (mm::comm::alHasError(status)) {
      continue;  // error indicator set — treat as not contributing
    }
    const bool hasOutputs = device.flatPdoMapping().outputBits > 0;
    const bool hasInputs = device.flatPdoMapping().inputBits > 0;
    int contribution =
        mm::comm::workingCounterContribution(mm::comm::alState(status), hasOutputs, hasInputs);
    // A device being commanded counts for the lower of where it is and where it is going. Down is
    // immediate — it stops answering the moment it leaves — while up is not true until it arrives,
    // and an errored device is left out either way: a transition is not a promise the error clears.
    if (target && std::ranges::find(transitioning, device.slavePosition()) != transitioning.end()) {
      contribution = std::min(contribution,
                              mm::comm::workingCounterContribution(*target, hasOutputs, hasInputs));
    }
    expected += contribution;
  }
  return expected;
}

void DeviceManager::updateExpectedWkc() {
  pd_->expectedWkc.store(expectedWkcDuring({}, std::nullopt), std::memory_order_relaxed);
}

void DeviceManager::lowerExpectedWkc(std::span<const uint16_t> positions,
                                     mm::comm::EtherCatState target) {
  // expectedWkcDuring already never predicts a rise, so this can only go down; the comparison is
  // here so that stays true of the *store* even if the live figure is recomputed in between.
  const int duringTransition = expectedWkcDuring(positions, target);
  if (duringTransition < pd_->expectedWkc.load(std::memory_order_relaxed)) {
    pd_->expectedWkc.store(duringTransition, std::memory_order_relaxed);
  }
}

std::expected<std::vector<DeviceStateInfo>, std::string> DeviceManager::transitionToState(
    const std::vector<uint16_t>& positions, mm::comm::EtherCatState targetState,
    std::chrono::steady_clock::duration timeout) {
  // busOperationMutex_ only, and this is the operation the split exists for. Driving AL state
  // blocks for seconds (the driver polls each slave to the target with a timeout), and holding a
  // lock readers share for all of it stalls the monitoring sampler and with it every
  // /api/monitorings endpoint. Holding busOperationMutex_ excludes the other control-plane
  // operations (which is what correctness needs: no concurrent scan/reset/re-map) while leaving
  // readers and borrowers untouched. set->driver and set->devices are stable throughout because
  // only an operation holding this mutex can rebuild them.
  const std::lock_guard busOperationLock(busOperationMutex_);
  const std::shared_ptr<DeviceSet> set = deviceSet();
  if (!set->driver) {
    return std::unexpected("no driver — call init() first");
  }
  if (set->devices.empty()) {
    return std::unexpected("no devices — call scan() first");
  }
  auto resolved = resolveTargets(positions);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  std::vector<uint16_t> targets = std::move(*resolved);

  // Reject illegal AL transitions before doing anything else. Entering SAFE-OP/OP re-maps by
  // reading PDO mappings over the CoE mailbox, which a device still in BOOT cannot serve — that
  // read segfaults inside SOEM. Validate every target against the EtherCAT state machine here so a
  // bad jump never reaches the mapper. Read the live state first (a plain AL-status register read,
  // valid in any state) rather than trusting the cache, since this guards a crash.
  auto currentStates = set->driver->readStates(targets);
  if (!currentStates) {
    return std::unexpected(currentStates.error());
  }
  for (std::size_t i = 0; i < targets.size(); ++i) {
    const auto currentState = mm::comm::alState((*currentStates)[i].alStatus);
    if (!isValidStateTransition(currentState, targetState)) {
      return std::unexpected(std::format(
          "illegal AL transition for device {}: {} -> {} (allowed: single-step climb "
          "INIT -> PRE-OP -> SAFE-OP -> OP, any drop, or BOOT only from/to INIT)",
          targets[i], mm::comm::toString(currentState), mm::comm::toString(targetState)));
    }
  }

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
    // PDO mapping re-read (a firmware update or a manual re-map can change it). A device
    // already exchanging being re-commanded (SAFE-OP -> OP) needs no re-map; the published image
    // still describes it. Re-mapping briefly pauses exchange for the whole bus (stopExchange
    // inside configureProcessData) — the accepted cost of bringing a device back online.
    const bool anyJoining = std::ranges::any_of(targets, [&set](uint16_t pos) {
      const Device* d = set->find(pos);
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
      for (const Device& d : set->devices) {
        const bool targeted = std::ranges::find(targets, d.slavePosition()) != targets.end();
        if (!targeted && mm::comm::alState(set->driver->slaveState(d.slavePosition())) ==
                             mm::comm::EtherCatState::Op) {
          opStayers.push_back(d.slavePosition());
        }
      }
      if (!opStayers.empty()) {
        spdlog::info(
            "Re-map pauses the whole bus — dropping {} staying OP device(s) to SAFE-OP first to "
            "avoid a sync-manager watchdog fault",
            opStayers.size());
        // These devices keep exchanging as they drop, so the expectation has to come down with
        // them or the drop we just chose to perform is counted against the bus.
        lowerExpectedWkc(opStayers, mm::comm::EtherCatState::SafeOp);
        set->driver->transitionToState(opStayers, std::nullopt, mm::comm::EtherCatState::SafeOp,
                                       timeout);
      }

      // Already holding busOperationMutex_ — call the mapping primitive directly, not the public
      // configureProcessData (which would re-acquire the non-recursive lock and deadlock).
      if (auto r = remapProcessImage(); !r) {
        return std::unexpected("auto-configure process data failed: " + r.error());
      }

      // Restore the staying devices to OP. The freshly published image already describes them, so
      // no further re-map is needed; the running GameLoop feeds PDO during the wait.
      if (!opStayers.empty()) {
        set->driver->transitionToState(opStayers, mm::comm::EtherCatState::SafeOp,
                                       mm::comm::EtherCatState::Op, timeout);
      }
    }
  } else if (processDataConfigured()) {
    // Leaving the exchange states. Tear the whole-bus image down only when no device will remain
    // exchanging afterwards; otherwise keep it published so the devices staying in SAFE-OP/OP
    // keep running while the targeted ones drop out. The targeted devices' working-counter share
    // is removed by the updateExpectedWkc() below, so health still reflects the live bus.
    const bool anyStays = std::ranges::any_of(set->devices, [&targets](const Device& d) {
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
      // Unpublishing is the point here; nothing is freed, so a stalled cycle changes nothing about
      // the safety of what follows. The image stays retained either way.
      static_cast<void>(stopExchange());
    }
  }

  spdlog::debug("transitionToState -> 0x{:02X} for {} device(s)", static_cast<int>(targetState),
                targets.size());
  // A drop that leaves other devices exchanging keeps the image published, so the RT loop runs
  // throughout the seconds this call takes. Bring the expectation down first: the targets stop
  // answering the moment they leave, and updateExpectedWkc below only catches up once they have.
  lowerExpectedWkc(targets, targetState);
  set->driver->transitionToState(targets, std::nullopt, targetState, timeout);

  // The driver call only logs failures, so read the settled state back and return it.
  // Callers derive "reached the target" as (!error && alState == targetState) per device.
  auto raw = set->driver->readStates(targets);
  if (!raw) {
    return std::unexpected(raw.error());
  }
  // readStates() refreshed the driver's cached AL status for every slave, so
  // Device::mailboxActive() / exchangesProcessData() now read through to the fresh state — no
  // per-device copy to sync.
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
      const Device* device = set->find(info.slavePosition);
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

  // Read the object dictionary of any device that just reached an exchange-capable state
  // (CoE mailbox live from PRE-OP up) and has no parameters yet, so recorder dumps, monitoring, and
  // the Parameters page have object names and data types without a manual read. Definitions only
  // (no value uploads) and cache-first, so a given device model pays the (slow, hundreds of
  // SDO-Info round-trips) enumeration only once — every later scan of the same hardware is an
  // instant cache load. Done here under the same exclusive lock as the module reconcile above; the
  // RT loop (lock-free PDO) and the WebSocket (separate loop) are unaffected — only other
  // control-plane calls wait, and only during that one-time first read.
  if (config_.readObjectDictionaryOnPreop) {
    for (const auto& info : result) {
      Device* device = set->find(info.slavePosition);
      if (!device || !device->mailboxActive() || device->hasParameters()) {
        continue;
      }
      // Attempted once per scan, not once per transition. The CoE mailbox is live from PRE-OP up,
      // so without this a device whose enumeration failed pays for it again on SAFE-OP and again on
      // OP — three passes over a bus that has already said it cannot answer, which on a large chain
      // is minutes. Deliberately not a retry-with-backoff: the explicit reads (POST
      // .../parameters/init) are the way back, and they clear this on success.
      if (device->parametersUnavailable()) {
        spdlog::debug(
            "Device {}: skipping the object-dictionary read on reaching {} — an earlier "
            "read failed; use POST /api/devices/{}/parameters/init to retry",
            info.slavePosition, mm::comm::toString(targetState), info.slavePosition);
        continue;
      }
      if (auto r = device->initializeParameters(/*readValues=*/false); !r) {
        // Names the state actually reached: the CoE mailbox is live from PRE-OP up, so this block
        // runs on entry to SAFE-OP and OP too. It also says that this was the only automatic
        // attempt and how to ask for another, because that is the one thing the device cannot
        // convey afterwards — from here on it simply looks like a device with no parameters.
        spdlog::warn(
            "Device {}: object-dictionary read on reaching {} failed: {}. It will not be "
            "attempted again automatically — use POST /api/devices/{}/parameters/init to retry",
            info.slavePosition, mm::comm::toString(targetState), r.error(), info.slavePosition);
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

std::expected<std::vector<DeviceStateInfo>, std::string> DeviceManager::deviceStates(
    const std::vector<uint16_t>& positions) {
  // Shared, like every other position-based read surface: this dereferences set->driver and
  // resolves positions against set->devices, both of which the exclusive rebuilders replace.
  const std::shared_ptr<DeviceSet> set = deviceSet();
  if (!set->driver) {
    return std::unexpected("no driver — call init() first");
  }
  auto resolved = resolveTargets(positions);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  std::vector<uint16_t> targets = std::move(*resolved);
  auto raw = set->driver->readStates(targets);
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

std::expected<std::vector<DeviceDiagnosticsInfo>, std::string> DeviceManager::deviceDiagnostics(
    const std::vector<uint16_t>& positions) {
  const std::shared_ptr<DeviceSet> set = deviceSet();
  if (!set->driver) {
    return std::unexpected("no driver — call init() first");
  }
  auto resolved = resolveTargets(positions);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  std::vector<uint16_t> targets = std::move(*resolved);
  auto raw = set->driver->readDiagnostics(targets);
  if (!raw) {
    return std::unexpected(raw.error());
  }
  std::vector<DeviceDiagnosticsInfo> result;
  result.reserve(raw->size());
  for (auto& d : *raw) {
    std::string name;
    if (const Device* device = set->find(d.slavePosition)) {
      name = device->name();
    }
    result.push_back(DeviceDiagnosticsInfo{.diagnostics = d, .deviceName = std::move(name)});
  }
  return result;
}

std::expected<std::vector<DcSyncInfo>, std::string> DeviceManager::dcSync(
    const std::vector<uint16_t>& positions) {
  const std::shared_ptr<DeviceSet> set = deviceSet();
  if (!set->driver) {
    return std::unexpected("no driver — call init() first");
  }
  auto resolved = resolveTargets(positions);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  std::vector<uint16_t> targets = std::move(*resolved);
  auto raw = set->driver->readDcSync(targets);
  if (!raw) {
    return std::unexpected(raw.error());
  }
  std::vector<DcSyncInfo> result;
  result.reserve(raw->size());
  for (auto& d : *raw) {
    std::string name;
    if (const Device* device = set->find(d.slavePosition)) {
      name = device->name();
    }
    result.push_back(DcSyncInfo{.dcSync = d, .deviceName = std::move(name)});
  }
  return result;
}

std::expected<mm::comm::ProcessDataWatchdogConfig, std::string> DeviceManager::processDataWatchdog(
    uint16_t slavePosition) {
  const std::shared_ptr<DeviceSet> set = deviceSet();
  if (!set->driver) {
    return std::unexpected("no driver — call init() first");
  }
  if (!set->find(slavePosition)) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  return set->driver->processDataWatchdog(slavePosition);
}

std::expected<mm::comm::ProcessDataWatchdogConfig, std::string>
DeviceManager::setProcessDataWatchdog(uint16_t slavePosition, std::chrono::nanoseconds timeout) {
  const std::shared_ptr<DeviceSet> set = deviceSet();
  if (!set->driver) {
    return std::unexpected("no driver — call init() first");
  }
  if (!set->find(slavePosition)) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  return set->driver->setProcessDataWatchdog(slavePosition, timeout);
}

std::expected<void, std::string> DeviceManager::initializeDeviceParameters(uint16_t slavePosition,
                                                                           bool readValues) {
  // The snapshot is what makes this safe to run for minutes: readObjectDictionary enumerates the
  // whole dictionary and releases the driver's controlPlaneMutex_ between SDO-Info transactions, so
  // the device and the driver must stay constructed across all of it. A concurrent scan publishes a
  // new set and this enumeration finishes against the retired one, whose next transaction fails.
  // Nothing waits: not the RT loop, not a rescan, not another reader.
  const std::shared_ptr<DeviceSet> set = deviceSet();
  Device* device = set->find(slavePosition);
  if (!device) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  return device->initializeParameters(readValues, config_.useCompleteAccess);
}

std::expected<void, std::string> DeviceManager::readAllDeviceParameters(uint16_t slavePosition) {
  // Shared lock, like readDeviceParameter/deviceParameterView: serialise against the exclusive
  // mutators that rebuild set->devices/set->driver so the device pointer stays valid for the sweep,
  // while still allowing concurrent off-thread reads. Device::readAllParameters re-takes the
  // per-device parametersMutex_ per entry, so this holds only the shared bus lock for the duration.
  const std::shared_ptr<DeviceSet> set = deviceSet();
  Device* device = set->find(slavePosition);
  if (!device) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  return device->readAllParameters(config_.useCompleteAccess);
}

std::optional<std::vector<uint8_t>> ProcessData::readPdo(uint16_t slavePosition, uint16_t index,
                                                         uint8_t subindex) const {
  const ProcessImage* img = image.load(std::memory_order_acquire);
  if (!img) {
    return std::nullopt;  // no image published — caller uses SDO
  }
  auto loc = img->find(slavePosition, index, subindex);
  if (!loc) {
    return std::nullopt;  // not PDO-mapped — caller uses SDO
  }
  if (loc->isOutput) {
    // Our own setpoint: always valid, no health gate, lock-free. It lives in the owning parameter's
    // cell, which is also what the composer sends — so a read here and the wire cannot disagree.
    const DeviceParameter* parameter = img->outputs[loc->entryIndex].parameter;
    if (parameter == nullptr) {
      return std::nullopt;  // dictionary not enumerated — caller uses SDO
    }
    return parameter->rawValueBytes();
  }
  // Input: gate on health, then read the newest recorded cycle (ring head()-1). On a lost or
  // partial frame the driver leaves the prior cycle's bytes in the IOmap, so an unhealthy bus
  // reads stale — signal the caller to read the authoritative SDO value. readRecord can only fail
  // if the producer lapped us by a whole ring (capacity cycles) between this head() load and the
  // copy — impossible at the configured depth (minutes of cycles), and head()-1 is the newest,
  // longest-lived slot anyway. If it ever does (a degenerate tiny ring), the SDO fallback below is
  // authoritative, so a single read needs no retry.
  if (!healthy()) {
    return std::nullopt;
  }
  const uint64_t head = ring.head();
  if (head == 0) {
    return std::nullopt;  // nothing recorded yet — caller uses SDO
  }
  ProcessDataRing::Record record;
  if (!ring.readRecord(head - 1, record)) {
    return std::nullopt;  // raced a full ring wrap (≈never) — caller uses SDO
  }
  return extractBits(std::span<const uint8_t>(record.inputs.data(), record.inputs.size()),
                     loc->bitOffset, loc->bitLength);
}

bool ProcessData::isOutputMapped(uint16_t slavePosition, uint16_t index, uint8_t subindex) const {
  const ProcessImage* img = image.load(std::memory_order_acquire);
  if (!img) {
    return false;
  }
  auto loc = img->find(slavePosition, index, subindex);
  // Only outputs are driven from the master. The value itself is already in the parameter's cell,
  // which the RT composer reads each cycle — writers to different objects never contend, and
  // bit-packed objects sharing a byte stay safe because one thread composes them. Same object =
  // last-writer-wins.
  return loc.has_value() && loc->isOutput;
}

std::optional<DeviceManager::PdoSampleSpec> DeviceManager::pdoSampleSpec(uint16_t slavePosition,
                                                                         uint16_t index,
                                                                         uint8_t subindex) const {
  const std::shared_ptr<DeviceSet> set = deviceSet();
  const ProcessImage* image = pd_->image.load(std::memory_order_acquire);
  if (!image) {
    return std::nullopt;
  }
  auto loc = image->find(slavePosition, index, subindex);
  if (!loc) {
    return std::nullopt;  // not PDO-mapped in the published image
  }
  const Device* device = set->find(slavePosition);
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
  const std::shared_ptr<DeviceSet> set = deviceSet();
  const Device* device = set->find(slavePosition);
  return device != nullptr && device->exchangesProcessData();
}

std::expected<DeviceParameter, std::string> DeviceManager::deviceParameterView(
    uint16_t slavePosition, uint16_t index, uint8_t subindex, bool refreshFromBus) {
  // Shared lock for the same reason as readDeviceParameter: serialise against the exclusive
  // mutators that rebuild set->devices, so the device pointer and its parameter map stay valid for
  // the refresh + copy below.
  const std::shared_ptr<DeviceSet> set = deviceSet();
  Device* device = set->find(slavePosition);
  if (!device) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  // refreshFromBus == false is the ?source=cache path: serve the cached struct without any bus I/O.
  // refreshFromBus == true (?source=auto) first syncs the cache via the live PDO image or an SDO
  // upload (routing lives in Device::readParameter), then returns the updated struct.
  if (refreshFromBus) {
    if (auto r = device->readParameter(index, subindex); !r) {
      return std::unexpected(r.error());
    }
  }
  auto copy = device->parameter(index, subindex);
  if (!copy) {
    return std::unexpected(std::format("device {}: parameter 0x{:04X}:{:02X} not found",
                                       slavePosition, index, subindex));
  }
  return *copy;
}

std::expected<DeviceParameter, std::string> DeviceManager::writeDeviceParameter(
    uint16_t slavePosition, uint16_t index, uint8_t subindex,
    const DeviceParameterValue& newValue) {
  // Shared lock: serialise against the exclusive mutators that rebuild set->devices/set->driver, so
  // a write that lands here off the control-plane thread can never see a half-torn device set. It
  // spans the read-back too, which is the point — see the header.
  const std::shared_ptr<DeviceSet> set = deviceSet();
  Device* device = set->find(slavePosition);
  if (!device) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  // Routing (stage into the output image when exchanging + output-mapped, SDO otherwise) lives in
  // Device. This entry point only resolves the position and takes the shared lock so an off-thread
  // caller is serialised against a device-set rebuild.
  if (auto written = device->writeParameter(index, subindex, newValue); !written) {
    return std::unexpected(std::move(written.error()));
  }
  auto updated = device->parameter(index, subindex);
  if (!updated) {
    // Unreachable, and reported rather than dereferenced: writeParameter resolved this parameter a
    // line ago, a device's parameter map is insert-only for its lifetime, and the shared lock spans
    // both halves — so an empty optional here means one of those three no longer holds.
    return std::unexpected(
        std::format("device {}: parameter 0x{:04X}:{:02X} vanished after a "
                    "successful write",
                    slavePosition, index, subindex));
  }
  return *updated;
}

std::expected<void, std::string> DeviceManager::writeDevicePdoMapping(uint16_t slavePosition,
                                                                      const PdoMapping& mapping) {
  // A control-plane operation, not a plain position-based write: rewriting 0x1C12/0x1C13 and the
  // 0x16xx/0x1Axx records is a multi-SDO sequence, and its read-back refreshes the device's
  // flatPdoMapping_ — the same field remapProcessImage() rewrites and buildProcessImage() reads.
  // busOperationMutex_ makes it exclusive against a concurrent re-map and against another write to
  // the same device, which a shared lock alone never did. The mailbox writes themselves are still
  // serialised per transaction by the driver's socket mutex, and the process image is not touched
  // here — a subsequent transitionToState back to SAFE-OP/OP re-reads the mapping and re-maps.
  const std::lock_guard busOperationLock(busOperationMutex_);
  const std::shared_ptr<DeviceSet> set = deviceSet();
  Device* device = set->find(slavePosition);
  if (!device) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  return device->writePdoMapping(mapping);
}

std::vector<OutputStageResult> DeviceManager::stageProcessDataOutputs(
    std::span<const OutputStageRequest> requests) {
  // One shared lock for the whole batch (per-item writeParameter takes each device's own
  // parametersMutex_): serialise against the exclusive mutators that rebuild
  // set->devices/set->driver, so every item in the batch sees one consistent device set and a
  // stable published image.
  const std::shared_ptr<DeviceSet> set = deviceSet();
  const ProcessImage* image = pd_->image.load(std::memory_order_acquire);
  std::vector<OutputStageResult> results;
  results.reserve(requests.size());
  for (const auto& req : requests) {
    OutputStageResult r;
    r.slavePosition = req.slavePosition;
    r.index = req.index;
    r.subindex = req.subindex;
    Device* device = set->find(req.slavePosition);
    if (!device) {
      r.error = "device " + std::to_string(req.slavePosition) + " not found";
      results.push_back(std::move(r));
      continue;
    }
    // Whether this object will land in the cyclic output image is exactly the gate writeParameter
    // stages on: output-mapped in the published image AND the device exchanging. Capture it before
    // the write so we can report staged vs written-but-not-cyclic with a precise reason.
    auto loc = image ? image->find(req.slavePosition, req.index, req.subindex) : std::nullopt;
    const bool outputMapped = loc && loc->isOutput;
    const bool exchanging = device->exchangesProcessData();
    if (auto w = device->writeParameter(req.index, req.subindex, req.value); !w) {
      r.error = w.error();
      results.push_back(std::move(r));
      continue;
    }
    if (outputMapped && exchanging) {
      r.staged = true;
    } else if (!exchanging) {
      r.error = "device not exchanging (not in SAFE-OP/OP) — value written but not cyclically sent";
    } else {
      r.error = "object not output-mapped — value written via SDO, not cyclically sent";
    }
    results.push_back(std::move(r));
  }
  return results;
}

void to_json(nlohmann::json& j, const OutputStageResult& result) {
  j = {{"slavePosition", result.slavePosition},
       {"index", result.index},
       {"subindex", result.subindex},
       {"staged", result.staged},
       {"error", result.error}};
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
       {"shortWkcCycles", info.shortWkcCycles},
       {"firstShortWkcUs", info.firstShortWkcUs},
       {"lastShortWkcUs", info.lastShortWkcUs},
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
       {"productName", info.productName},
       {"vendorId", info.vendorId},
       {"productCode", info.productCode},
       {"revisionNumber", info.revisionNumber},
       {"serialNumber", info.serialNumber},
       {"configuredAddress", c.configuredAddress},
       {"aliasAddress", c.aliasAddress},
       {"outputBits", c.outputBits},
       {"inputBits", c.inputBits},
       {"mailbox",
        {{"writeLength", c.mailbox.writeLength},
         {"writeOffset", c.mailbox.writeOffset},
         {"readLength", c.mailbox.readLength},
         {"readOffset", c.mailbox.readOffset},
         {"protocols", c.mailbox.protocols},
         {"coeDetails", c.mailbox.coeDetails},
         {"foeDetails", c.mailbox.foeDetails},
         {"eoeDetails", c.mailbox.eoeDetails},
         {"soeDetails", c.mailbox.soeDetails}}},
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

void to_json(nlohmann::json& j, const DeviceManager& dm) {
  // The snapshot keeps the vector alive for the conversion; a concurrent scan publishes another.
  j = dm.deviceSet()->devices;
}

}  // namespace mm::node
