#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <format>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "node/device.h"
#include "node/parameter_cache.h"
#include "node/process_data_ring.h"
#include "node/process_image.h"

namespace mm::node {

/// @brief Holds the RT process-data runtime state (the recorder ring, the published image
///        pointer, the in-cycle depth counter, and the RT scratch buffers). Defined in the .cc —
///        pimpl'd so its non-movable members and large fixed buffers stay out of the header.
struct ProcessData;

/// @brief Current AL state snapshot for a single device.
struct DeviceStateInfo {
  uint16_t slavePosition;  ///< 1-based position on the fieldbus.
  uint16_t alStatus;       ///< Raw AL Status register (bits 3:0 = state, bit 4 = error indicator).
  uint16_t alState;  ///< AL state decoded from alStatus (1=Init, 2=PreOp, 3=Boot, 4=SafeOp, 8=Op).
  bool error;        ///< True when the AL Status error indicator bit is set.
  uint16_t alStatusCode;  ///< AL Status Code (ETG.1000.6 §6.4.1); non-zero when error is true.
};

/// @brief Serialises a DeviceStateInfo to JSON.
void to_json(nlohmann::json& j, const DeviceStateInfo& info);

/// @brief One mapped object in the published process image, with its name resolved.
///
/// A flattened, API-facing view of a @c ProcessImageEntry: the raw entry plus the textual
/// name looked up from the owning device's parameter map (empty when the object dictionary
/// has not been enumerated for that device).
struct ProcessImageObjectInfo {
  uint16_t slavePosition = 0;  ///< 1-based bus position of the owning device.
  uint16_t index = 0;          ///< CoE object index.
  uint8_t subindex = 0;        ///< CoE object subindex.
  std::string name;            ///< Object name, or empty if the OD has not been enumerated.
  uint32_t bitOffset = 0;      ///< Absolute bit offset within the direction's image.
  uint16_t bitLength = 0;      ///< Width of the value in bits.
};

/// @brief API-facing snapshot of the currently published process image and its runtime health.
///
/// Built on the calling (non-RT) thread by @c DeviceManager::processImageInfo. When an image is
/// live (@c configured true) the layout describes it. When no image is published (@c configured
/// false) but generations have been mapped since the last reset(), the byte sizes and object
/// lists describe the most recent retained generation — the last-known layout — so a bus that has
/// dropped out of SAFE-OP/OP remains inspectable; @c lastWkc then holds the final exchange value
/// while @c expectedWkc reflects the now-idle bus. The lists are empty only before any image has
/// ever been mapped.
struct ProcessImageInfo {
  bool configured = false;   ///< Whether an image is currently published for exchange.
  uint32_t outputBytes = 0;  ///< Size of the output image (master→slave).
  uint32_t inputBytes = 0;   ///< Size of the input image (slave→master).
  int expectedWkc = 0;       ///< Working counter expected from the devices currently exchanging.
  int lastWkc = 0;           ///< Working counter from the most recent exchange (0 before any).
  bool healthy = false;      ///< Whether the last working counter meets the expected value.
  /// Cycles that answered short since the bus came up. @c healthy is a sample and can miss
  /// a fault that has already cleared; this cannot.
  uint64_t shortWkcCycles = 0;
  uint64_t firstShortWkcUs = 0;  ///< Epoch microseconds of the first such cycle (0 if none).
  uint64_t lastShortWkcUs = 0;   ///< Epoch microseconds of the most recent one (0 if none).
  std::size_t generations = 0;   ///< Number of process images retained since the last reset().
  std::vector<ProcessImageObjectInfo> outputs;  ///< Output-mapped objects in image order.
  std::vector<ProcessImageObjectInfo> inputs;   ///< Input-mapped objects in image order.
};

/// @brief Serialises a ProcessImageObjectInfo to JSON.
void to_json(nlohmann::json& j, const ProcessImageObjectInfo& obj);

/// @brief Serialises a ProcessImageInfo to JSON.
void to_json(nlohmann::json& j, const ProcessImageInfo& info);

/// @brief One object to stage into the output process image, as parsed from a batch write request.
///
/// The @c value is coerced to the object's declared data type by the write path
/// (@c Device::writeParameter), so callers pass a loosely-typed @c DeviceParameterValue.
struct OutputStageRequest {
  uint16_t slavePosition = 0;  ///< 1-based bus position of the owning device.
  uint16_t index = 0;          ///< CoE object index.
  uint8_t subindex = 0;        ///< CoE object subindex.
  DeviceParameterValue value;  ///< Value to write; coerced to the object's declared type.
};

/// @brief Per-object outcome of a batch output stage.
///
/// @c staged is true only when the value landed in the published output image and so will be sent
/// on the next exchange cycle. When false, @c error explains why (unknown device, coercion failure,
/// object not output-mapped, or the bus not exchanging — in the latter two cases the value is still
/// written via SDO/cache, just not cyclically driven).
struct OutputStageResult {
  uint16_t slavePosition = 0;  ///< Echoes the request.
  uint16_t index = 0;          ///< Echoes the request.
  uint8_t subindex = 0;        ///< Echoes the request.
  bool staged = false;         ///< Whether the value was staged into the cyclic output image.
  std::string error;           ///< Empty on success; otherwise why the object was not staged.
};

/// @brief Serialises an OutputStageResult to JSON.
void to_json(nlohmann::json& j, const OutputStageResult& result);

/// @brief A slave's static ESC configuration plus its resolved device name.
///
/// API-facing wrapper around @c mm::comm::SlaveConfig that adds the human-readable device
/// name (from the device set, empty when no matching device is known), built by
/// @c DeviceManager::busConfig on the (non-RT) caller's thread.
struct SlaveConfigInfo {
  mm::comm::SlaveConfig config;  ///< Raw ESC configuration as captured by the driver.
  std::string deviceName;        ///< SII device name for this slave position, empty if unknown.
  std::string productName;       ///< Canonical product name (distinguishes SOMANET products where
                                 ///< deviceName is the generic "SOMANET"); empty if unknown.
  // Immutable identity, denormalised from the Device for this slave position (like deviceName) so
  // the Configuration page can show it without a second endpoint. Zero when the slave is unknown.
  uint32_t vendorId = 0;        ///< Vendor ID from EEPROM.
  uint32_t productCode = 0;     ///< Product code from EEPROM.
  uint32_t revisionNumber = 0;  ///< Revision number from EEPROM.
  uint32_t serialNumber = 0;    ///< Serial number from EEPROM.
};

/// @brief Serialises a SlaveConfigInfo (and its nested SM/FMMU/mailbox/DC) to JSON.
void to_json(nlohmann::json& j, const SlaveConfigInfo& info);

/// @brief A slave's live ESC health diagnostics plus its resolved device name.
///
/// API-facing wrapper around @c mm::comm::SlaveDiagnostics that adds the human-readable device
/// name (from the device set, empty when no matching device is known), built by
/// @c DeviceManager::deviceDiagnostics on the (non-RT) caller's thread.
struct DeviceDiagnosticsInfo {
  mm::comm::SlaveDiagnostics diagnostics;  ///< Decoded ESC counters as read by the driver.
  std::string deviceName;  ///< Device name for this slave position, empty if unknown.
};

/// @brief Serialises a DeviceDiagnosticsInfo (and its nested per-port counters) to JSON.
void to_json(nlohmann::json& j, const DeviceDiagnosticsInfo& info);

/// @brief A slave's live distributed-clock sync status plus its resolved device name.
///
/// API-facing wrapper around @c mm::comm::DcSyncDiagnostics that adds the human-readable device
/// name (from the device set, empty when no matching device is known), built by
/// @c DeviceManager::dcSync on the (non-RT) caller's thread.
struct DcSyncInfo {
  mm::comm::DcSyncDiagnostics dcSync;  ///< Decoded DC sync status as read by the driver.
  std::string deviceName;              ///< Device name for this slave position, empty if unknown.
};

/// @brief Serialises a DcSyncInfo to JSON.
void to_json(nlohmann::json& j, const DcSyncInfo& info);

/// @brief Runtime tuning passed to @c DeviceManager::init. Every field has a sensible default, so a
///        caller can pass @c {} (or omit it) and override only what it cares about; new knobs can
///        be added here without changing the @c init signature.
struct DeviceManagerConfig {
  /// Read a device's object dictionary when it first reaches PRE-OP (definitions only, cache-first)
  /// so dumps/monitoring/Parameters have names and data types without a manual read. See
  /// @c transitionToState.
  bool readObjectDictionaryOnPreop = true;
  /// Read multi-subindex objects with a single CoE Complete Access upload during a parameter value
  /// read, falling back to per-subindex reads on slaves that do not support it. See
  /// @c Device::initializeParameters.
  bool useCompleteAccess = true;
  /// Directory for `.mmpd` recorder dumps. Empty resolves at dump time to a @c "dumps"
  /// subdirectory of the per-user cache directory (@c mm::core::userCacheDir) — not the OS
  /// temporary directory, which is reaped on a timer. Passed through to @c dumpProcessData.
  std::string recorderDumpDir;
  /// Depth of the process-data recorder ring in cycles/rows; the ring is allocated at
  /// @c configureProcessData to hold exactly this many cycles, independent of the loop period.
  uint32_t recorderCapacity = 300000;
};

/// @brief One published generation of the bus: the driver that owns the socket, and the devices
///        found on it.
///
/// A set is built by @c init or @c scan, published once, and never modified after that. Publishing
/// freezes the @c devices vector, not the devices in it — each @c Device guards its own parameter
/// map with its own mutex.
///
/// **The set is what makes device lifetime a refcount rather than a lock.** Every off-RT caller
/// works through a @c std::shared_ptr to one (@c DeviceManager::deviceSet), so a @c Device& stays
/// valid for as long as the caller holds that pointer. A concurrent @c scan does not wait for the
/// caller and does not invalidate the reference: it publishes a *new* set, and the old one dies
/// when its last holder drops it. The retired devices keep working against their own driver, which
/// the set also owns, so a transfer against hardware that is no longer on the bus fails cleanly
/// instead of touching freed memory.
///
/// The RT thread does not touch the @c shared_ptr, because @c std::atomic<std::shared_ptr<T>> is
/// not lock-free (and libc++ does not implement it at all). It reads a raw published pointer
/// instead, drained by @c ProcessData::pauseCycle exactly as the process image is.
struct DeviceSet {
  /// The driver every device in this set talks through. Shared, so a retired set keeps it alive.
  std::shared_ptr<mm::comm::FieldbusDriver> driver;
  /// The devices in bus order: index 0 is position 1. Frozen once the set is published.
  std::vector<Device> devices;
  /// The value @c DeviceManager::topologyGeneration reported when this set was published.
  uint64_t topologyGeneration = 0;

  /// @brief Device at @p slavePosition, or @c nullptr when the set holds none.
  Device* find(uint16_t slavePosition);
  /// @brief Const overload of @c find. Same contract.
  const Device* find(uint16_t slavePosition) const;
};

/// @brief One device, plus the set that keeps it alive.
///
/// What @c DeviceManager::deviceAt hands back. The handle *is* the lifetime: while you hold it, the
/// @c Device it names stays constructed and its parameter map stays put, whatever @c scan or
/// @c reset do meanwhile. A bare @c deviceSet()->find(pos) would dangle the moment the temporary
/// set pointer died, which is the mistake this type exists to make unwritable.
///
/// Falsy when the position resolved to no device. Dereferencing then is undefined, exactly as for a
/// null pointer.
class DeviceHandle {
 public:
  DeviceHandle() = default;
  DeviceHandle(std::shared_ptr<DeviceSet> set, Device* device)
      : set_(std::move(set)), device_(device) {}

  /// @brief Whether a device was found.
  explicit operator bool() const { return device_ != nullptr; }
  Device& operator*() const { return *device_; }
  Device* operator->() const { return device_; }
  /// @brief The device, or @c nullptr. For a caller that must pass it on as a pointer.
  Device* get() const { return device_; }

 private:
  std::shared_ptr<DeviceSet> set_;
  Device* device_ = nullptr;
};

/// @brief The error to return when a bus position resolves to no device.
///
/// One wording, so every surface answers the same way. Converts to any
/// @c std::expected<T, std::string>.
inline std::unexpected<std::string> deviceNotFound(uint16_t slavePosition) {
  return std::unexpected(std::format("device {} not found", slavePosition));
}

/// @brief Owns the fieldbus driver and node collection, and drives PDO exchange.
///
/// The driver is not required at construction — call @c init() to supply one.
/// This allows the app to start without a driver and be initialised later via
/// the HTTP API. Injected into @c GameLoop (for @c exchangeProcessData) and
/// @c HttpServer (for SDO/state operations).
class DeviceManager {
 public:
  DeviceManager();

  /// @brief Defined out-of-line so @c unique_ptr<ProcessData> can hold an incomplete type.
  ~DeviceManager();

  /// @brief Takes ownership of @p driver and initialises it.
  ///
  /// Must be called before @c scan() and @c exchangeProcessData(). One-shot: fails if a
  /// driver is already held — call @c reset() first. Replacing a live driver
  /// would dangle the @c FieldbusDriver& that every @c Device holds.
  ///
  /// @param driver  Concrete fieldbus driver to own and operate.
  /// @param config  Runtime tuning (recorder depth, cycle period). Defaults are used for any field
  ///        left at its default; omit the argument entirely to use all defaults.
  /// @return Void on success, or an error string if a driver is already held or
  ///         driver initialisation fails.
  std::expected<void, std::string> init(std::unique_ptr<mm::comm::FieldbusDriver> driver,
                                        const DeviceManagerConfig& config = {});

  /// @brief Sets the on-disk parameter-cache policy and location.
  ///
  /// The cache is a process-level concern (its directory comes from the config file, like the
  /// ports), independent of whether a driver is initialised — so this is called once at startup,
  /// not from @c init. The configured location is then correct for the management API and for every
  /// device's @c initializeParameters regardless of init order. Safe to call again to re-apply.
  void configureParameterCache(const ParameterCacheConfig& config);

  /// @brief The on-disk parameter cache, for the management API (list / download / remove).
  ///
  /// A disk store independent of the bus; usable with or without a driver initialised.
  const ParameterCache& parameterCache() const;

  /// @brief Scans the bus for nodes and populates the device list.
  ///
  /// Must be called after @c init(). Forwards to @c FieldbusDriver::scan().
  ///
  /// @return Number of nodes found on success, or an error string on failure.
  std::expected<int, std::string> scan();

  /// @brief Stops the fieldbus driver and clears the device list.
  ///
  /// Transitions all slaves to INIT state, closes the network interface, and
  /// removes all @c Device objects. After this returns, @c init() and
  /// @c scan() may be called again. Must not be called while @c exchangeProcessData()
  /// is running concurrently.
  void reset();

  /// @brief Whether a driver is currently held (i.e. @c init() has succeeded and
  ///        @c reset() has not since been called). Thread-safe.
  bool initialised() const;

  /// @brief Whether a device holds @p slavePosition. Thread-safe; no bus access.
  ///
  /// For the caller that only needs to tell "no such device" from a failure of the operation
  /// itself — an HTTP route answering 404 before it starts. It is a point-in-time answer, so it
  /// does not entitle the caller to a @c Device&: use @c deviceAt for that.
  bool hasDevice(uint16_t slavePosition) const;

  /// @brief The device at @p slavePosition, as a handle that keeps it alive. Falsy if absent.
  ///
  /// The way to reach a device from outside @c DeviceManager. It resolves the position in the
  /// current @c DeviceSet and hands back a @c DeviceHandle holding both. Keep the handle for as
  /// long as you work with the device — a millisecond or ten minutes, it makes no difference, and
  /// no lock is held either way.
  ///
  /// A @c scan that lands while you hold one publishes a *new* set. Your device keeps working, and
  /// its next bus transaction fails against hardware that has moved or gone. That is the intended
  /// outcome: a rescan never waits for a procedure, and a procedure never reads freed memory.
  ///
  /// @code
  /// const auto device = deviceManager.deviceAt(slavePosition);
  /// if (!device) { return deviceNotFound(slavePosition); }
  /// return device->readParameter(index, subindex);
  /// @endcode
  DeviceHandle deviceAt(uint16_t slavePosition) const;

  /// @brief The current device set, as a pointer that keeps it alive. Never @c nullptr.
  ///
  /// One mutex acquisition, long enough to copy a @c shared_ptr, then nothing. For work that spans
  /// several devices — a JSON listing, a bus-wide walk. @c deviceAt is the one-device form. Before
  /// the first @c init the set is empty, not absent.
  std::shared_ptr<DeviceSet> deviceSet() const;

  /// @brief Holds a cyclic task's whole body open against a device-set rebuild. RT-safe.
  ///
  /// **`GameLoop` constructs one for you, around the whole task list, every cycle. A cyclic task
  /// never writes one.** It is public because the loop lives in another library, not because a task
  /// author has anything to do with it.
  ///
  /// A cyclic task resolves its own devices and parameters (@c findDevice / @c
  /// Device::findParameter) rather than being handed them, so it must not run while @c scan or
  /// @c reset is replacing the device set it walks. The loop therefore enters the cycle first and
  /// calls no task at all when the guard is falsy:
  ///
  /// @code
  /// const DeviceManager::CycleGuard cycle(deviceManager_);   // in GameLoop::run
  /// if (cycle) {
  ///   for (CyclicTask* task : tasks_) { task->execute(ctx); }
  /// }
  /// @endcode
  ///
  /// **It is the published process image that decides.** Falsy means no image is published, which
  /// is the bus's "not activated" state — the same two-phase model every EtherCAT stack uses
  /// (configure, then activate; reconfiguring means deactivating first). Every control-plane
  /// operation that rebuilds the device set already unpublishes the image and then drains via
  /// @c stopExchange, so a guard taken after the unpublish fails and one taken before is waited
  /// out. The RT thread never blocks: it runs no tasks for those cycles, which is what the bus is
  /// doing anyway.
  ///
  /// Never blocks, never allocates: one atomic increment and one atomic load.
  ///
  /// @warning Holding one across a control-plane call (@c scan, @c reset, @c transitionToState)
  ///          deadlocks that call against its own drain. A cyclic task makes no such call, and the
  ///          loop holds the guard for nothing else.
  class CycleGuard {
   public:
    /// @brief Enters the cycle. Falsy if the bus is not activated — run no device work.
    explicit CycleGuard(DeviceManager& deviceManager);
    /// @brief Leaves the cycle, if this guard entered it.
    ~CycleGuard();

    CycleGuard(const CycleGuard&) = delete;
    CycleGuard& operator=(const CycleGuard&) = delete;

    /// @brief Whether this guard entered the cycle and device access is safe now.
    explicit operator bool() const { return entered_; }

   private:
    ProcessData* processData_;
    bool entered_;
  };

  /// @brief Device lookup by bus position. O(N) over a handful of devices; @c nullptr if absent.
  ///
  /// **For the RT thread.** It reads the raw published device set and takes no lock, which is what
  /// makes it callable from a cyclic task. Off the RT thread use @c deviceAt or @c deviceSet,
  /// which hold the set alive by refcount.
  ///
  /// **Lifetime.** The returned pointer — and any @c DeviceParameter* obtained through it — is
  /// valid for the body of one cycle, which is the scope @c GameLoop already holds the cycle open
  /// for. A @c scan or @c reset publishes a new set and drops the RT reference to the old one, so a
  /// cyclic task must re-resolve each cycle and never cache a @c Device* across cycles.
  ///
  /// **Position is not identity.** Inserting a device into the chain shifts every position after
  /// it, so a task pinned to position 4 can silently find different hardware there after a rescan.
  /// Capture @c topologyGeneration() when binding and abandon the work when it changes.
  Device* findDevice(uint16_t slavePosition);

  /// @brief Const overload of @c findDevice. Same contract.
  const Device* findDevice(uint16_t slavePosition) const;

  /// @brief Monotonic counter bumped every time the device set is rebuilt (@c scan / @c reset).
  ///
  /// An off-thread consumer that pinned work to a device position records this value and
  /// compares it on each use: a change means the topology was re-scanned (or cleared) and a
  /// previously-valid position may now name a different device or none at all, so the consumer
  /// must re-validate. Lock-free (atomic). Starts at 0; the first @c scan makes it 1.
  uint64_t topologyGeneration() const;

  /// @brief Maps process data and publishes the process image for exchange.
  ///
  /// Calls @c FieldbusDriver::configureProcessData (which maps the IOmap and lays out the
  /// FMMUs), reads each device's PDO mapping via SDO, assembles a @c ProcessImage that resolves
  /// every mapped object to an absolute position *and* to its owning parameter's cell, and
  /// publishes the image so @c exchangeProcessData begins exchanging. All devices must be in
  /// PRE-OP (mailbox active) for the mapping reads to succeed.
  ///
  /// Re-runnable: a later call (e.g. after a device returns from a firmware download)
  /// re-maps the whole bus and republishes. While re-mapping it first unpublishes the image
  /// so @c exchangeProcessData becomes a no-op; call it with the GameLoop stopped or drained,
  /// as for @c init / @c reset (see the @c exchangeProcessData warning).
  ///
  /// @return Void on success, or an error string if no driver is initialised, no devices
  ///         have been discovered, the driver mapping fails, a device's mapping cannot be
  ///         read, or the assembled image is inconsistent with the driver layout.
  std::expected<void, std::string> configureProcessData();

  /// @brief Exchanges one cycle of process data: sends the current outputs, captures inputs.
  ///
  /// Composes the output image from each output object's parameter cell, calls
  /// @c FieldbusDriver::exchangeProcessData, appends the cycle (both directions) to the recorder
  /// ring for non-RT readers, and decodes every mapped input back into its own cell — which is what
  /// makes @c Device::value<T>() a single atomic load rather than an image lookup.  No-op until
  /// @c configureProcessData has published an image, so the GameLoop can call it unconditionally
  /// every cycle.  Runs on the RT thread and takes no lock.
  ///
  /// @warning @c exchangeProcessData() runs on the RT GameLoop thread while @c init(),
  ///          @c scan(), @c reset(), and @c configureProcessData() may be called from the
  ///          HTTP server thread.  The published-image pointer gates exchange off during a
  ///          re-map, and the device set it reads is the raw published pointer, which the control
  ///          plane replaces only with the cycle drained.  Every control-plane operation drains
  ///          before it mutates, so no caller has to stop the loop first.
  void exchangeProcessData();

  /// @brief Whether @c configureProcessData has published a process image for exchange.
  bool processDataConfigured() const;

  /// @brief Monotonic counter bumped every time a new process image is published (each (re)map).
  ///
  /// A consumer that captured per-object layout (bit offsets/lengths) from the image compares
  /// this on each use: a change means the bus was re-mapped and offsets may have shifted, so the
  /// captured layout must be refreshed. Lock-free (atomic). Distinct from @c topologyGeneration
  /// (scan/reset), which signals the device set itself changed.
  uint64_t processImageGeneration() const;

  /// @brief The working counter from the most recent process-data exchange (0 before any).
  int lastWorkingCounter() const;

  /// @brief The working counter expected from the devices currently exchanging (SAFE-OP/OP).
  ///
  /// Computed from each device's PDO presence and current AL state, so it tracks a partially
  /// operational bus: an OP device with outputs and inputs contributes 3, a SAFE-OP device
  /// contributes 1 (inputs only), and PRE-OP/below contribute 0. Recomputed on configure and
  /// on each state transition — not the whole-bus all-OP figure.
  int expectedWorkingCounter() const;

  /// @brief Whether every device that should be exchanging is contributing to the cycle.
  ///
  /// True when process data is configured and the last working counter is at least the
  /// expected value. A drop below expected means a device that should be in SAFE-OP/OP stopped
  /// contributing (cable, fault, or an unexpected state change).
  bool processDataHealthy() const;

  /// @brief Snapshots the published process image and its runtime health for the API.
  ///
  /// Resolves every mapped object to an absolute bit offset and a name (looked up from each
  /// device's parameter map, empty if the OD has not been enumerated) and reports the byte
  /// sizes, expected/last working counter, health, and the number of retained image
  /// generations. Returns @c configured false with empty object lists when no image is
  /// published. Runs on the (non-RT) caller's thread; reads the published image lock-free.
  ProcessImageInfo processImageInfo() const;

  /// @brief Serialises the recorder's current span to a `.mmpd` dump file and returns its path.
  ///
  /// Freezes the recorder span at the instant of the call — @c [recorderOldestSeq(),
  /// recorderHead()) — and writes every cycle in it (full raw inputs + outputs, sequence,
  /// epoch-ns timestamp) plus the process image embedded as a header, so the file decodes fully
  /// offline. Works in any state: while exchanging (OP/SAFE-OP) it dumps tail→head at that moment
  /// and ignores cycles the producer records afterwards; after teardown it uses the most recent
  /// retained image generation. The header objects' names/data types come from each device's
  /// parameter map (empty/0 when the object dictionary has not been enumerated).
  ///
  /// The file is written to the configured @c recorderDumpDir (created if absent); an empty
  /// @c recorderDumpDir (the default) resolves to a @c "dumps" subdirectory of the per-user cache
  /// directory. The filename is @c dump-<UTC-timestamp>-<endSequence>.mmpd. Only the path is
  /// returned, but because the default location sits under the user-cache root, the written file
  /// is reachable through the @c /api/user-cache endpoints — listed, downloaded and deleted
  /// remotely rather than only from a shell on the host.
  ///
  /// @return The absolute path of the written file, or an error string if no image has ever been
  ///         mapped, the recorder is empty, or the directory/file could not be written.
  std::expected<std::string, std::string> dumpProcessData();

  /// @brief Serialises the recorder dump into an in-memory buffer — the same `.mmpd` bytes
  /// @c dumpProcessData writes to disk — for streaming over HTTP.
  ///
  /// Buffers the whole span in memory (a deep recorder ring can be large; true row-by-row
  /// streaming is a future optimisation). Same image/span semantics as @c dumpProcessData; the RT
  /// producer is never blocked. Runs on the (non-RT) caller's thread.
  ///
  /// @return The dump bytes, or an error string if no image has ever been mapped or the recorder
  ///         is empty.
  std::expected<std::string, std::string> dumpProcessDataBuffer();

  /// @brief Snapshots each slave's static ESC configuration (SM, FMMU, mailbox, DC) for the API.
  ///
  /// Passes through the driver's cached configuration (no bus I/O) and resolves each slave's
  /// device name. Returns an empty vector when no driver is set or the transport has no ESC
  /// (e.g. SPoE). Runs on the (non-RT) caller's thread.
  std::vector<SlaveConfigInfo> busConfig() const;

  /// @brief Transitions a set of devices to @p targetState, blocking until all arrive or
  ///        @p timeout elapses, and reports the final state of each.
  ///
  /// If @p positions is empty, all discovered devices are targeted. After the transition
  /// settles, the final AL state of every targeted device is read back and returned — so
  /// callers see exactly where each device ended up rather than a bare success flag. A
  /// device reached the target when its returned snapshot has @c error clear and
  /// @c alState equal to @p targetState; otherwise @c alStatusCode explains why.
  ///
  /// Must be called after both @c init() and @c scan().
  ///
  /// @param positions    1-based slave positions to transition; empty = all devices.
  /// @param targetState  Desired EtherCAT AL state.
  /// @param timeout      Maximum time to wait for all devices.
  /// @return The final state snapshot of each targeted device (in the order targeted), or an
  ///         error string if no driver is initialised, no devices have been discovered, or the
  ///         final state read-back fails.
  std::expected<std::vector<DeviceStateInfo>, std::string> transitionToState(
      const std::vector<uint16_t>& positions, mm::comm::EtherCatState targetState,
      std::chrono::steady_clock::duration timeout);

  /// @brief Reads the current AL state for a set of devices.
  ///
  /// If @p positions is empty, all discovered devices are queried.
  /// Must be called after both @c init() and @c scan().
  ///
  /// @param positions  1-based slave positions to query; empty = all devices.
  /// @return AL state snapshot per device, or an error string if the driver is
  ///         not initialised or the hardware read fails.
  std::expected<std::vector<DeviceStateInfo>, std::string> deviceStates(
      const std::vector<uint16_t>& positions);

  /// @brief Reads live ESC health diagnostics (link quality, error counters, watchdogs) for a set
  ///        of devices, resolving each slave's device name.
  ///
  /// If @p positions is empty, all discovered devices are queried. Forwards to
  /// @c FieldbusDriver::readDiagnostics (FPRD reads — not cached) and wraps each result with its
  /// device name. The returned counters are monotonic since the last clear, so a diagnostics page
  /// polls this and watches for a rising delta rather than an absolute value.
  ///
  /// Must be called after both @c init() and @c scan().
  ///
  /// @param positions  1-based slave positions to query; empty = all devices.
  /// @return Per-device diagnostics in the order targeted, or an error string if no driver is
  ///         initialised, the transport has no ESC, or a register read fails.
  std::expected<std::vector<DeviceDiagnosticsInfo>, std::string> deviceDiagnostics(
      const std::vector<uint16_t>& positions);

  /// @brief Reads live distributed-clock synchronisation status for a set of devices, resolving
  ///        each slave's device name.
  ///
  /// If @p positions is empty, all discovered devices are queried. Forwards to
  /// @c FieldbusDriver::readDcSync (FPRD reads — not cached) and wraps each result with its device
  /// name. The system-time difference is meaningful only while the bus is exchanging in
  /// SAFE-OP/OP; a page polls this and watches for a slave whose deviation from the reference
  /// clock stays large or grows rather than converging toward zero.
  ///
  /// Must be called after both @c init() and @c scan().
  ///
  /// @param positions  1-based slave positions to query; empty = all devices.
  /// @return Per-device DC sync status in the order targeted, or an error string if no driver is
  ///         initialised, the transport has no ESC, or a register read fails.
  std::expected<std::vector<DcSyncInfo>, std::string> dcSync(
      const std::vector<uint16_t>& positions);

  /// @brief Reads the process-data (sync-manager) watchdog configuration of one device.
  ///
  /// Forwards to @c FieldbusDriver::processDataWatchdog after resolving the device. Returns the
  /// configured timeout itself (the divider/time registers), not the expiration counter that
  /// @c deviceDiagnostics surfaces.
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @return The decoded watchdog configuration, or an error string if no driver is initialised,
  ///         the device is unknown, the transport has no ESC, or a register read fails.
  std::expected<mm::comm::ProcessDataWatchdogConfig, std::string> processDataWatchdog(
      uint16_t slavePosition);

  /// @brief Sets the process-data (sync-manager) watchdog timeout of one device.
  ///
  /// Forwards to @c FieldbusDriver::setProcessDataWatchdog after resolving the device. A larger
  /// timeout lets a device tolerate the brief PDO pause of a whole-bus re-map without faulting;
  /// a @p timeout of zero disables the watchdog. The achieved timeout is rounded to the device's
  /// watchdog tick base — the returned config reports what was programmed. The write persists
  /// until the ESC reloads EEPROM (power cycle).
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @param timeout        Desired watchdog timeout; zero disables the watchdog.
  /// @return The watchdog configuration actually programmed, or an error string if no driver is
  ///         initialised, the device is unknown, the transport has no ESC, @p timeout is
  ///         unrepresentable, or a register access fails.
  std::expected<mm::comm::ProcessDataWatchdogConfig, std::string> setProcessDataWatchdog(
      uint16_t slavePosition, std::chrono::nanoseconds timeout);

  /// @brief Enumerates the CoE object dictionary of one device and populates its
  ///        parameter map. See @c Device::initializeParameters for details.
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @param readValues     When @c true, also issue an SDO upload for each entry
  ///                       and store the decoded value on the parameter.
  /// @return Void on success, an error string if the device is unknown, or if
  ///         the OD enumeration itself fails.
  std::expected<void, std::string> initializeDeviceParameters(uint16_t slavePosition,
                                                              bool readValues);

  /// @brief Refreshes the cached value of every readable parameter of one device, in place.
  ///
  /// Unlike @c initializeDeviceParameters this does not re-enumerate the object dictionary — it
  /// re-reads the values of the parameters already loaded (PDO-aware per entry). See
  /// @c Device::readAllParameters for the per-entry semantics. Best-effort: a single object that
  /// fails to read is logged and the call still succeeds.
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @return Void on success, or an error string if the device is unknown or has no parameters
  ///         loaded yet.
  std::expected<void, std::string> readAllDeviceParameters(uint16_t slavePosition);

  /// @brief Convenience: finds a device by position and reads one of its parameters.
  ///
  /// Equivalent to @c findDevice(slavePosition)->readParameter(index, subindex) — see
  /// @c Device::readParameter for the online/offline semantics. Saves callers a manual
  /// lookup when they only have a position.
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @param index          CoE object index.
  /// @param subindex       CoE object subindex.
  /// @return The value, or an error string if the device or parameter is unknown, or the
  ///         (online) SDO upload fails.
  std::expected<DeviceParameterValue, std::string> readDeviceParameter(uint16_t slavePosition,
                                                                       uint16_t index,
                                                                       uint8_t subindex);

  /// @brief Returns a full parameter struct (value + metadata), optionally refreshed from the bus.
  ///
  /// Backs the @c GET /api/devices/{pos}/parameters/{index}/{subindex} route. When
  /// @p refreshFromBus is @c true (the @c ?source=auto default) the cache is first synced via the
  /// live PDO image or an SDO upload (routing lives in @c Device::readParameter); when @c false
  /// (the @c ?source=cache path) the cached struct is returned without any bus I/O. Either way the
  /// returned value is a copy taken under the parameter-cache lock, so it is safe off-thread.
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @param index          CoE object index.
  /// @param subindex       CoE object subindex.
  /// @param refreshFromBus When @c true, sync the cache from the device before copying.
  /// @return The parameter, or an error string if the device or parameter is unknown, or (when
  ///         refreshing) the read fails.
  std::expected<DeviceParameter, std::string> deviceParameterView(uint16_t slavePosition,
                                                                  uint16_t index, uint8_t subindex,
                                                                  bool refreshFromBus);

  /// @brief Convenience: finds a device by position, writes one of its parameters, returns it.
  ///
  /// Equivalent to @c findDevice(slavePosition)->writeParameter(index, subindex, value) —
  /// see @c Device::writeParameter for the online/offline semantics (offline edits succeed
  /// and are held as @c SyncState::Pending).
  ///
  /// **The updated parameter is returned rather than left for the caller to read back**, because
  /// the write coerces the value to the declared type and moves @c syncState: a caller that wants
  /// either has to ask, and asking in a second call would resolve the position again. A @c scan
  /// landing in that gap publishes a new set, so the read-back would fail for a write that
  /// succeeded — leaving the caller to answer with something other than the parameter it is
  /// expected to produce. One call over both halves removes the gap.
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @param index          CoE object index.
  /// @param subindex       CoE object subindex.
  /// @param value          Value to set; coerced to the parameter's declared type.
  /// @return The parameter as it stands after the write — coerced value and resulting
  ///         @c syncState — or an error string if the device or parameter is unknown, the value
  ///         cannot be coerced, or an online download fails.
  std::expected<DeviceParameter, std::string> writeDeviceParameter(
      uint16_t slavePosition, uint16_t index, uint8_t subindex,
      const DeviceParameterValue& newValue);

  /// @brief Convenience: finds a device by position and writes its PDO mapping.
  ///
  /// Equivalent to @c findDevice(slavePosition)->writePdoMapping(mapping) — see
  /// @c Device::writePdoMapping for the procedure and the PRE-OP requirement. Does not itself
  /// re-map the process image: the caller drives the device back to SAFE-OP/OP afterwards, and
  /// @c transitionToState re-reads the mapping and rebuilds the whole-bus image at that point.
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @param mapping        Desired output (RxPDO) and input (TxPDO) mapping, in assignment order.
  /// @return Void on success, or an error string if the device is unknown, is not in PRE-OP, or the
  ///         mapping write/verify fails.
  std::expected<void, std::string> writeDevicePdoMapping(uint16_t slavePosition,
                                                         const PdoMapping& mapping);

  /// @brief Convenience: finds a device by position and reads its PDO mapping grouped by object.
  ///
  /// Equivalent to @c findDevice(slavePosition)->readPdoMapping() — see @c Device::readPdoMapping.
  /// Reads fresh over SDO (mailbox must be active: PRE-OP/SAFE-OP/OP). Used by the read side of the
  /// PDO-mapping route and to echo the verified read-back after a write.
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @return The grouped mapping, or an error string if the device is unknown or a read fails.
  std::expected<PdoMapping, std::string> readDevicePdoMapping(uint16_t slavePosition);

  /// @brief Stages a batch of output objects into the process image in one call.
  ///
  /// Backs @c POST @c /api/process-data/outputs, the "send all" action of the Process Data page:
  /// the user edits several outputs and ships them together. Each request is routed through
  /// @c Device::writeParameter (same coercion + PDO-staging path as @c writeDeviceParameter), and
  /// each gets an @c OutputStageResult reporting whether it actually landed in the cyclic output
  /// image — so the UI can flag any object that was not staged without failing the whole batch.
  ///
  /// Best-effort atomicity: the slots are stored sequentially on this (non-RT) thread, so a batch
  /// can straddle two consecutive RT cycles (≤1 ms skew worst case). Composing every slot in a
  /// single cycle would require RT-side generation gating, which would break the lock-free output
  /// path — out of scope; the brief skew is acceptable for manual process-data writes.
  ///
  /// @param requests  The objects to stage, each with a value coerced to its declared type.
  /// @return One result per request, in the same order.
  std::vector<OutputStageResult> stageProcessDataOutputs(
      std::span<const OutputStageRequest> requests);

  // --- Off-thread sampling read surface (for monitoring) ---
  //
  // These let the monitoring sampler read live values from its own thread without ever blocking
  // on the bus: SDO objects from the (refresher-fed) cache via value(); PDO objects decoded by
  // the caller from recorder-ring records (recorderHead()/recorderOldestSeq()/readRecord()) using
  // the layout pdoSampleSpec() captures. Thread-safe; they hand back copies, never device pointers.

  /// @brief Returns a copy of a parameter's cached value, no bus access. Thread-safe.
  ///
  /// For monitoring's SDO objects: the value the @c ParameterRefresher last wrote to the cache.
  ///
  /// @return The cached value, or @c nullopt if the device or parameter is unknown.
  std::optional<DeviceParameterValue> value(uint16_t slavePosition, uint16_t index,
                                            uint8_t subindex) const;

  /// @brief Everything needed to decode one PDO object from a raw process-image snapshot:
  ///        which image it lives in, where, and how wide / what type.
  struct PdoSampleSpec {
    bool isOutput;       ///< True if the object is in the output image (RxPDO), false for input.
    uint32_t bitOffset;  ///< Absolute bit offset within that direction's image.
    uint16_t bitLength;  ///< Width of the value in bits.
    uint16_t dataType;   ///< ETG.1020 data-type code, for decoding the extracted bytes.
  };

  /// @brief Resolves a PDO-mapped object's decode spec from the published image. Thread-safe.
  ///
  /// The sampler captures this once per PDO parameter (and re-captures when
  /// @c processImageGeneration changes), then for each recorded cycle reads the matching region
  /// of the ring record (@c inputs for an input object, @c outputs for an output) and runs
  /// @c extractBits + @c decodeSdoBytes itself — so the bus is never touched on the sampling path
  /// and every value in a row comes from the same cycle's record.
  ///
  /// @return The spec, or @c nullopt if no image is published, the object is not PDO-mapped, or
  ///         its data type is unknown (object dictionary not enumerated).
  std::optional<PdoSampleSpec> pdoSampleSpec(uint16_t slavePosition, uint16_t index,
                                             uint8_t subindex) const;

  /// @brief The recorder ring's next sequence number; @c recorderHead()-1 is the newest recorded
  ///        cycle and @c recorderHead()==0 means nothing has been recorded yet.
  ///
  /// A monitoring holds a read cursor and ships every record in @c [cursor, recorderHead()) per
  /// flush, advancing the cursor — a non-destructive reader that never gates the RT producer.
  ///
  /// Thread-safe against both writers of the ring, which are not the same adversary and do not
  /// need the same protection. @c ProcessDataRing is lock-free against the RT producer's
  /// @c write(); this and the two accessors below additionally take @c processDataMutex_ shared,
  /// because the *control plane* also writes the ring — @c allocate / @c clear release its storage
  /// on a re-map, @c scan and @c reset — and those take it exclusively. Reading it without that
  /// lock is a use-after-free rather than a torn read.
  uint64_t recorderHead() const;

  /// @brief The oldest sequence number still present in the ring (@c max(0, head - capacity)).
  ///        A cursor below this has been lapped (overwritten) and must resync to it. Thread-safe
  ///        on the same terms as @c recorderHead.
  uint64_t recorderOldestSeq() const;

  /// @brief Copies the recorded cycle for @p seq into @p out. Thread-safe on the same terms as
  ///        @c recorderHead (the shared lock is taken per record, not per span).
  ///
  /// @return @c true if @p seq is present and copied without a concurrent overwrite; @c false if
  ///         it is no longer in the ring or the copy raced the producer (the caller skips it).
  bool readRecord(uint64_t seq, ProcessDataRing::Record& out) const;

  /// @brief Whether the device at @p slavePosition is currently exchanging (SAFE-OP/OP).
  ///        Thread-safe; reads the driver's cached AL state, no bus I/O.
  ///
  /// The monitoring sampler's per-device live gate: only an exchanging device has live process
  /// data, so a non-exchanging device's parameters sample @c null. @c false for an unknown
  /// position.
  bool deviceExchangesProcessData(uint16_t slavePosition) const;

 private:
  /// @brief Decodes every mapped input from the just-exchanged image into its parameter's cell.
  ///
  /// The RT half of the value path: called on the game-loop thread at the end of
  /// @c exchangeProcessData, once the input bytes are in hand. Non-allocating, lock-free and
  /// lookup-free — each entry carries the parameter it belongs to, resolved when the image was
  /// built. See the definition for why it is not gated on the working counter.
  void decodeInputsIntoCells(const ProcessImage& image);

  /// @brief Rows written and the @c [startSeq, endSeq) sequence span of a serialised dump.
  struct DumpSpan {
    uint64_t rows = 0;
    uint64_t startSeq = 0;
    uint64_t endSeq = 0;
  };

  /// @brief Serialises the current recorder span as a `.mmpd` byte stream to @p out.
  ///
  /// Shared by @c dumpProcessData (file) and @c dumpProcessDataBuffer (in-memory). Holds @c
  /// processDataMutex_ shared for the whole serialisation (the ring is read while held; the RT
  /// producer appends lock-free and is never blocked). @p out must be seekable — @c
  /// writeProcessDataDump patches the row count after streaming the rows.
  std::expected<DumpSpan, std::string> serializeDump(std::ostream& out);

  /// @brief (Re)maps the whole-bus process image and publishes it for exchange.
  ///
  /// The core mapping primitive: drains exchange, has the driver map the IOmap, re-reads each
  /// device's PDO mapping, builds the @c ProcessImage, and publishes it — with nothing to seed,
  /// since each output object's value already lives in its own parameter's cell, which is what the
  /// composer reads. **The caller must hold @c busOperationMutex_**, which is what keeps the
  /// published set from changing underneath it; the ring re-allocation takes @c processDataMutex_
  /// exclusively for its own brief window. Two callers compose it: the public
  /// @c configureProcessData and @c transitionToState (when a (re)joining device requires a
  /// re-map).
  std::expected<void, std::string> remapProcessImage();

  /// @brief Resolves a caller-supplied position list to validated bus positions.
  ///
  /// An empty list expands to every discovered device (in bus order). A non-empty list is
  /// validated against the device set — any position that is not a discovered device is
  /// rejected — so untrusted caller input can never reach the driver's slave-indexed
  /// accessors (which index a fixed-size slave array without bounds-checking) out of range.
  /// Call with a driver held; the device set defines the valid range.
  ///
  /// @param positions  1-based slave positions, or empty for all devices.
  /// @return The validated positions (or all device positions when empty), or an error
  ///         string naming the first position that is not a discovered device.
  std::expected<std::vector<uint16_t>, std::string> resolveTargets(
      const std::vector<uint16_t>& positions) const;

  /// @brief Sums each non-errored device's working-counter contribution, counting a device in
  ///        @p transitioning for the *lower* of where it is and the @p target it is being
  ///        commanded to.
  ///
  /// The whole of the expectation arithmetic lives here so the live figure and the mid-transition
  /// one cannot drift apart. With an empty @p transitioning it is simply the live figure.
  ///
  /// The lower of the two per device, not per bus: one call can carry a drop and a climb at once
  /// (`{1, 2} -> SAFE-OP` with 1 in OP and 2 in PRE-OP), and there the two would net out to a
  /// figure the bus does not reach until the climbing device arrives — counting the seconds in
  /// between as a fault, which is the very thing this exists to prevent.
  int expectedWkcDuring(std::span<const uint16_t> transitioning,
                        std::optional<mm::comm::EtherCatState> target) const;

  /// @brief Recomputes the expected working counter from the devices' current AL states and
  ///        PDO presence. Called after configure and after each state transition.
  void updateExpectedWkc();

  /// @brief Lowers the expectation to what the bus will answer once @p positions reach @p target,
  ///        if that is less than what is expected now.
  ///
  /// Called *before* commanding a drop, and it is what keeps a deliberate transition from being
  /// counted as a bus fault. A device commanded out of OP stops answering the moment it leaves,
  /// while @c updateExpectedWkc only runs once the transition has settled — so without this the RT
  /// loop spends the whole multi-second transition comparing the bus against an expectation that
  /// still includes a device we told to stop, and @c shortWkcCycles fills up with our own doing.
  ///
  /// Only ever *lowers*. Raising the expectation before the devices have got there would make
  /// every climb read as short instead, which is the same bug in the other direction; a climb is
  /// covered by @c updateExpectedWkc afterwards, and a bus answering more than expected is not
  /// counted at all.
  void lowerExpectedWkc(std::span<const uint16_t> positions, mm::comm::EtherCatState target);

  /// @brief Unpublishes the process image and waits for any in-flight exchange cycle to finish.
  ///
  /// After this returns, @c exchangeProcessData is a no-op and the RT thread is no longer
  /// touching the driver's IOmap, so it is safe to re-map or tear down. Bounded wait.
  void stopExchange();

  /// @brief Publishes @p set as the current generation. Call with @c busOperationMutex_ held.
  ///
  /// Swaps the shared pointer readers copy, then hands the RT thread the raw pointer. The caller
  /// must have drained the RT cycle first (@c stopExchange), because that is what makes replacing
  /// @c rtSet_ safe: no cyclic task can still be inside a @c CycleGuard reading the set it points
  /// at.
  void publishDeviceSet(std::shared_ptr<DeviceSet> set);

  // busOperationMutex_ — "one control-plane operation drives the bus at a time". It guards no
  // member: it is a mutual-exclusion token over an *activity*, which is what distinguishes it from
  // the driver's controlPlaneMutex_ (that one serialises a single socket transaction; this one
  // serialises a whole multi-transaction operation). Held for their entire duration by
  // init/scan/reset/configureProcessData/transitionToState/writeDevicePdoMapping, the only
  // operations that publish a device set, re-map the process image, drive AL state, or rewrite a
  // device's PDO mapping. No reader ever takes it, so a multi-second AL transition excludes only
  // other control-plane callers — never the monitoring sampler, and never a procedure that is
  // already running.
  mutable std::mutex busOperationMutex_;

  // currentSetMutex_ — guards the shared_ptr below, and nothing it points to. Held for exactly one
  // pointer copy, which is why a reader can never be delayed by an operation: a shared_ptr copy is
  // not atomic against an assignment to the same object, and std::atomic<std::shared_ptr<T>> is not
  // lock-free (libc++ does not implement it at all). Device *lifetime* is the refcount's job, not
  // this lock's.
  mutable std::mutex currentSetMutex_;
  // The published generation of the bus. Never null: an empty set stands in before the first
  // init(), so every reader can dereference the pointer it gets without a check.
  std::shared_ptr<DeviceSet> currentSet_;

  // The same set as a raw pointer, for the RT thread, which must not touch a shared_ptr. Written by
  // the control plane only with the RT cycle drained (ProcessData::pauseCycle), so a cyclic task
  // inside a CycleGuard can dereference it for the whole body. rtSet_ is the strong reference that
  // keeps that object alive; the control plane replaces it only after a drain, so the set the RT
  // thread may still be reading is never the one being freed.
  std::atomic<DeviceSet*> publishedSet_{nullptr};
  std::shared_ptr<DeviceSet> rtSet_;

  // processDataMutex_ — the two non-atomic members of ProcessData: the recorder ring's storage and
  // the retained image generations. The ring is lock-free for one writer and many readers, but
  // ProcessDataRing::allocate and ::clear are a third kind of writer: they free the buffer, which
  // no sequence check on the reader side can survive. The generations vector has the same shape — a
  // re-map appends, scan/reset clear. Readers take this shared, those writers exclusively. A leaf:
  // nothing is ever acquired while it is held.
  mutable std::shared_mutex processDataMutex_;

  // Bumped on every scan()/reset(); see topologyGeneration().
  std::atomic<uint64_t> topologyGeneration_{0};
  // Bumped every time remapProcessImage() publishes a new image; see processImageGeneration().
  std::atomic<uint64_t> processImageGeneration_{0};
  std::unique_ptr<ProcessData> pd_;
  // On-disk parameter-definition cache, shared by every Device (handed to each by pointer at
  // scan()). Outlives every device set. Configured at init().
  ParameterCache parameterCache_;
  // Runtime tuning captured at init(); drives recorder-ring sizing at configureProcessData.
  DeviceManagerConfig config_;
};

/// @brief Serialises all devices in a DeviceManager to a JSON array.
///
/// Produces a JSON array where each element is the serialised form of a
/// `Device` (see `to_json(nlohmann::json&, const Device&)`), in bus order.
/// Participates in nlohmann ADL so that `nlohmann::json(deviceManager)` works.
///
/// @param j   Output JSON value.
/// @param dm  DeviceManager whose device list to serialise.
void to_json(nlohmann::json& j, const DeviceManager& dm);

}  // namespace mm::node
