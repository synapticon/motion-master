#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "node/device.h"
#include "node/process_data_ring.h"
#include "node/process_image.h"

namespace mm::node {

/// @brief Holds the RT process-data runtime state (the recorder ring, the published image
///        pointer, output staging slots, and scratch). Defined in the .cc — pimpl'd so its
///        non-movable members and large fixed buffers stay out of the header.
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
  uint16_t slavePosition;  ///< 1-based bus position of the owning device.
  uint16_t index;          ///< CoE object index.
  uint8_t subindex;        ///< CoE object subindex.
  std::string name;        ///< Object name, or empty if the OD has not been enumerated.
  uint32_t bitOffset;      ///< Absolute bit offset within the direction's image.
  uint16_t bitLength;      ///< Width of the value in bits.
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
  bool configured;          ///< Whether an image is currently published for exchange.
  uint32_t outputBytes;     ///< Size of the output image (master→slave).
  uint32_t inputBytes;      ///< Size of the input image (slave→master).
  int expectedWkc;          ///< Working counter expected from the devices currently exchanging.
  int lastWkc;              ///< Working counter from the most recent exchange (0 before any).
  bool healthy;             ///< Whether the last working counter meets the expected value.
  std::size_t generations;  ///< Number of process images retained since the last reset().
  std::vector<ProcessImageObjectInfo> outputs;  ///< Output-mapped objects in image order.
  std::vector<ProcessImageObjectInfo> inputs;   ///< Input-mapped objects in image order.
};

/// @brief Serialises a ProcessImageObjectInfo to JSON.
void to_json(nlohmann::json& j, const ProcessImageObjectInfo& obj);

/// @brief Serialises a ProcessImageInfo to JSON.
void to_json(nlohmann::json& j, const ProcessImageInfo& info);

/// @brief A slave's static ESC configuration plus its resolved device name.
///
/// API-facing wrapper around @c mm::comm::SlaveConfig that adds the human-readable device
/// name (from the device set, empty when no matching device is known), built by
/// @c DeviceManager::busConfig on the (non-RT) caller's thread.
struct SlaveConfigInfo {
  mm::comm::SlaveConfig config;  ///< Raw ESC configuration as captured by the driver.
  std::string deviceName;        ///< Device name for this slave position, empty if unknown.
};

/// @brief Serialises a SlaveConfigInfo (and its nested SM/FMMU/mailbox/DC) to JSON.
void to_json(nlohmann::json& j, const SlaveConfigInfo& info);

/// @brief A slave's live ESC health diagnostics plus its resolved device name.
///
/// API-facing wrapper around @c mm::comm::SlaveDiagnostics that adds the human-readable device
/// name (from the device set, empty when no matching device is known), built by
/// @c DeviceManager::getDeviceDiagnostics on the (non-RT) caller's thread.
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
/// @c DeviceManager::getDcSync on the (non-RT) caller's thread.
struct DcSyncInfo {
  mm::comm::DcSyncDiagnostics dcSync;  ///< Decoded DC sync status as read by the driver.
  std::string deviceName;              ///< Device name for this slave position, empty if unknown.
};

/// @brief Serialises a DcSyncInfo to JSON.
void to_json(nlohmann::json& j, const DcSyncInfo& info);

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
  /// @param recorderHistorySeconds  Depth of the process-data recorder ring in seconds; the ring
  ///        is allocated at @c configureProcessData to hold this many seconds of cycles.
  /// @param cyclePeriodUs  The GameLoop cycle period in microseconds, used with the history depth
  ///        to size the ring (capacity = seconds * 1e6 / periodUs).
  /// @return Void on success, or an error string if a driver is already held or
  ///         driver initialisation fails.
  std::expected<void, std::string> init(std::unique_ptr<mm::comm::FieldbusDriver> driver,
                                        uint32_t recorderHistorySeconds = 300,
                                        uint32_t cyclePeriodUs = 1000);

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
  ///        @c reset() has not since been called).
  bool initialised() const { return driver_ != nullptr; }

  /// @brief Returns the list of discovered devices.
  /// @return Devices in bus order (index 0 = node position 1). Empty before @c scan().
  const std::vector<Device>& devices() const;

  /// @brief Finds a device by its 1-based bus position.
  ///
  /// @param slavePosition  1-based position of the device on the fieldbus.
  /// @return Pointer to the matching @c Device, or @c nullptr if not found.
  const Device* findDevice(uint16_t slavePosition) const;

  /// @brief Mutable overload of @c findDevice.
  ///
  /// Hands back a writable @c Device so SDK callers can drive it directly —
  /// e.g. @c dm.findDevice(1)->writeValue(0x2030, 1, 123). @c nullptr if not found.
  ///
  /// @warning Not internally synchronised: call on the control-plane (server) thread, or
  /// while holding @c busMutex_. Off-thread consumers (monitoring) must go through the
  /// position-based methods such as @c readDeviceParameter, which look the device up under the
  /// lock and never hand out a pointer that @c scan / @c reset could dangle.
  Device* findDevice(uint16_t slavePosition);

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
  /// FMMUs), reads each device's PDO mapping via SDO, assembles a @c ProcessImage that
  /// resolves every mapped object to an absolute position, zero-initialises the output
  /// staging buffer, and publishes the image so @c exchangeProcessData begins exchanging.
  /// All devices must be in PRE-OP (mailbox active) for the mapping reads to succeed.
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

  /// @brief Exchanges one cycle of process data: sends staged outputs, captures inputs.
  ///
  /// Composes the output image from the per-object staging slots, calls
  /// @c FieldbusDriver::exchangeProcessData, and appends the cycle (both directions) to the
  /// recorder ring for non-RT readers.  No-op until @c configureProcessData has published an
  /// image, so the GameLoop can call it unconditionally every cycle.  Runs on the RT thread and
  /// takes no lock.
  ///
  /// @warning @c exchangeProcessData() runs on the RT GameLoop thread while @c init(),
  ///          @c scan(), @c reset(), and @c configureProcessData() may be called from the
  ///          HTTP server thread.  The published-image pointer gates exchange off during a
  ///          re-map, but @c driver_ / @c devices_ themselves are not otherwise locked across
  ///          that boundary.  Stop the loop (or drain one cycle) before calling @c init() /
  ///          @c reset() / @c configureProcessData() via the API.
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
  std::expected<std::vector<DeviceStateInfo>, std::string> getDeviceStates(
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
  std::expected<std::vector<DeviceDiagnosticsInfo>, std::string> getDeviceDiagnostics(
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
  std::expected<std::vector<DcSyncInfo>, std::string> getDcSync(
      const std::vector<uint16_t>& positions);

  /// @brief Reads the process-data (sync-manager) watchdog configuration of one device.
  ///
  /// Forwards to @c FieldbusDriver::processDataWatchdog after resolving the device. Returns the
  /// configured timeout itself (the divider/time registers), not the expiration counter that
  /// @c getDeviceDiagnostics surfaces.
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @return The decoded watchdog configuration, or an error string if no driver is initialised,
  ///         the device is unknown, the transport has no ESC, or a register read fails.
  std::expected<mm::comm::ProcessDataWatchdogConfig, std::string> getProcessDataWatchdog(
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

  /// @brief Reports whether a single device's CoE/SDO mailbox is currently active.
  ///
  /// Performs a live AL-state read for @p slavePosition and returns whether the device's
  /// mailbox is reachable — AL state PRE-OP, SAFE-OP, or OP. This is independent of the AL
  /// error indicator (a device in SAFE-OP+error still answers mailbox requests); only INIT
  /// and BOOT report inactive. Updates the driver's cached AL status as a side effect, exactly
  /// as @c getDeviceStates does. Reading one position at a time lets a single missing device
  /// report inactive without disturbing the others.
  ///
  /// Must be called after both @c init() and @c scan().
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @return @c true if the mailbox is active, @c false otherwise, or an error string if no
  ///         driver is initialised, the device is unknown, or the hardware read fails.
  std::expected<bool, std::string> isDeviceMailboxActive(uint16_t slavePosition);

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

  /// @brief Convenience: finds a device by position and writes one of its parameters.
  ///
  /// Equivalent to @c findDevice(slavePosition)->writeParameter(index, subindex, value) —
  /// see @c Device::writeParameter for the online/offline semantics (offline edits succeed
  /// and are held as @c SyncState::Pending).
  ///
  /// @param slavePosition  1-based bus position of the target device.
  /// @param index          CoE object index.
  /// @param subindex       CoE object subindex.
  /// @param value          Value to set; coerced to the parameter's declared type.
  /// @return Void on success, or an error string if the device or parameter is unknown,
  ///         the value cannot be coerced, or an online download fails.
  std::expected<void, std::string> writeDeviceParameter(uint16_t slavePosition, uint16_t index,
                                                        uint8_t subindex,
                                                        DeviceParameterValue value);

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
  ///        cycle and @c recorderHead()==0 means nothing has been recorded yet. Thread-safe.
  ///
  /// A monitoring holds a read cursor and ships every record in @c [cursor, recorderHead()) per
  /// flush, advancing the cursor — a non-destructive reader that never gates the RT producer.
  uint64_t recorderHead() const;

  /// @brief The oldest sequence number still present in the ring (@c max(0, head - capacity)).
  ///        A cursor below this has been lapped (overwritten) and must resync to it. Thread-safe.
  uint64_t recorderOldestSeq() const;

  /// @brief Copies the recorded cycle for @p seq into @p out. Thread-safe, lock-free.
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
  /// @brief (Re)maps the whole-bus process image and publishes it for exchange.
  ///
  /// The core mapping primitive: drains exchange, has the driver map the IOmap, re-reads each
  /// device's PDO mapping, builds the @c ProcessImage, seeds the output staging from cached
  /// parameter values, and publishes the image. **The caller must hold @c busMutex_
  /// exclusively.** Two callers compose it: the public @c configureProcessData (takes the lock,
  /// then calls this) and @c transitionToState (already holds the lock when a (re)joining
  /// device requires a re-map).
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

  /// @brief Recomputes the expected working counter from the devices' current AL states and
  ///        PDO presence. Called after configure and after each state transition.
  void updateExpectedWkc();

  /// @brief Unpublishes the process image and waits for any in-flight exchange cycle to finish.
  ///
  /// After this returns, @c exchangeProcessData is a no-op and the RT thread is no longer
  /// touching the driver's IOmap, so it is safe to re-map or tear down. Bounded wait.
  void stopExchange();

  // Guards the non-RT mutable state — driver_, devices_, and the retained image generations —
  // against the off-RT monitoring threads. Control-plane mutators (init/scan/reset/
  // configureProcessData/transitionToState) take it exclusively; the position-based value
  // reads called from other threads (readDeviceParameter/writeDeviceParameter) take it shared.
  // The RT exchangeProcessData() never takes it (it is gated by the atomic image pointer
  // instead), so the lock never touches the real-time path. Lock order, where both are taken:
  // busMutex_ (here) before any Device-level parametersMutex_.
  mutable std::shared_mutex busMutex_;
  // Bumped under the exclusive lock on every scan()/reset(); see topologyGeneration().
  std::atomic<uint64_t> topologyGeneration_{0};
  // Bumped every time remapProcessImage() publishes a new image; see processImageGeneration().
  std::atomic<uint64_t> processImageGeneration_{0};
  std::unique_ptr<mm::comm::FieldbusDriver> driver_;
  std::vector<Device> devices_;
  std::unique_ptr<ProcessData> pd_;
  // Recorder ring sizing, captured at init(): the ring (allocated at configureProcessData) holds
  // recorderHistorySeconds_ of cycles at the GameLoop period cyclePeriodUs_.
  uint32_t recorderHistorySeconds_ = 300;
  uint32_t cyclePeriodUs_ = 1000;
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
