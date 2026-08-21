#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "node/device.h"

namespace mm::node {

/// @brief Generic CANopen device-profile (CiA301) object indices — present on **any** CoE device,
///        independent of the CiA402 drive profile. Simple VAR objects are addressed at subindex 0;
///        RECORD objects (0x1010, 0x1011, 0x1016, 0x1018, 0x1023) at their documented sub-entries.
enum GenericObject : uint16_t {
  kDeviceType = 0x1000,     ///< UNSIGNED32, ro — mandatory; the device profile it speaks.
  kErrorRegister = 0x1001,  ///< UNSIGNED8, ro (volatile) — active error class bit field.
  kCobIdSync = 0x1005,      ///< INTEGER32, rw — COB-ID of the SYNC message.
  kCommunicationCyclePeriod = 0x1006,     ///< INTEGER32, rw — communication cycle period in µs.
  kManufacturerDeviceName = 0x1008,       ///< VISIBLE_STRING, ro — human-readable device name.
  kManufacturerSoftwareVersion = 0x100A,  ///< VISIBLE_STRING, ro — firmware/software version.
  kGuardTime = 0x100C,                    ///< UNSIGNED16, rw — node guarding guard time in ms.
  kLifeTimeFactor = 0x100D,               ///< UNSIGNED8, rw — node guarding life time factor.
  kStoreParameters = 0x1010,              ///< RECORD, rw — save parameters to non-volatile storage.
  kRestoreDefaultParameters = 0x1011,     ///< RECORD, rw — restore default parameters.
  kConsumerHeartbeatTime = 0x1016,        ///< RECORD, rw — consumer heartbeat time in ms.
  kProducerHeartbeatTime = 0x1017,        ///< UNSIGNED16, rw — producer heartbeat time in ms.
  kIdentity = 0x1018,                     ///< IDENTITY record, ro — vendor/product/revision/serial.
  kSynchronousCounterOverflowValue = 0x1019,  ///< UNSIGNED8, rw — SYNC counter overflow value.
  kOsCommand = 0x1023,      ///< RECORD — command (rw) / status + response (ro, volatile).
  kOsCommandMode = 0x1024,  ///< UNSIGNED8, wo — write-only, not readable; setter only.
};

/// @brief The CANopen identity object (0x1018) — the four UNSIGNED32 sub-entries that uniquely
///        identify a device on the bus.
struct Identity {
  uint32_t vendorId{};        ///< 0x1018:01 — EtherCAT vendor ID.
  uint32_t productCode{};     ///< 0x1018:02 — vendor-assigned product code.
  uint32_t revisionNumber{};  ///< 0x1018:03 — revision number.
  uint32_t serialNumber{};    ///< 0x1018:04 — device serial number.
};

/// @brief The restore-default-parameters object (0x1011) — one UNSIGNED32 per restorable group.
///        Reading a sub-entry reports the restore capability (bit 0 = device restores on command);
///        writing the "load" signature to it triggers the restore.
struct RestoreDefaultParameters {
  uint32_t all{};            ///< 0x1011:01 — restore all default parameters.
  uint32_t communication{};  ///< 0x1011:02 — restore communication default parameters.
  uint32_t application{};    ///< 0x1011:03 — restore application default parameters.
  uint32_t manufacturer{};   ///< 0x1011:04 — restore manufacturer-defined default parameters.
};

/// @brief Which group of default parameters to restore (which 0x1011 sub-entry to command). The
///        enum value @b is the sub-entry number, so it maps straight to the object subindex.
enum class RestoreGroup : uint8_t {
  kAll = 1,            ///< 0x1011:01 — all default parameters.
  kCommunication = 2,  ///< 0x1011:02 — communication default parameters.
  kApplication = 3,    ///< 0x1011:03 — application default parameters.
  kManufacturer = 4,   ///< 0x1011:04 — manufacturer-defined default parameters.
};

/// @brief Parses a restore-group token ("all" / "communication" / "application" / "manufacturer")
///        into a @c RestoreGroup. Returns @c std::nullopt for any other token.
std::optional<RestoreGroup> parseRestoreGroup(std::string_view token);

/// @brief The token naming @p group — the inverse of @c parseRestoreGroup, and the form the group
///        takes on the wire. Never returns @c nullptr.
constexpr std::string_view toString(RestoreGroup group) {
  switch (group) {
    case RestoreGroup::kAll:
      return "all";
    case RestoreGroup::kCommunication:
      return "communication";
    case RestoreGroup::kApplication:
      return "application";
    case RestoreGroup::kManufacturer:
      return "manufacturer";
  }
  return "unknown";
}

/// @brief Timing for the store-parameters confirmation walk (@c ProfileDevice::runStoreParameters).
///
/// After the "save" signature is written, the procedure waits @c settle for the device to begin the
/// flash write, then polls 0x1010:01 up to @c retries more times, @c interval apart, until it reads
/// back 1. The retry defaults (10 polls, 500 ms) match the reference client; @c settle (1 s) tracks
/// device flash-write behaviour and rarely needs overriding (tests pass zero delays to run
/// instantly).
struct StoreParametersConfig {
  uint32_t retries = 10;                    ///< Maximum confirmation polls after the first.
  std::chrono::milliseconds interval{500};  ///< Delay between confirmation polls.
  std::chrono::milliseconds settle{1000};   ///< Wait after the write before the first poll.

  /// @brief Abandons the confirmation wait; default never stops.
  ///
  /// It stops the *waiting*, not the store: the "save" signature is already on the wire and the
  /// device is already writing to flash, so a cancelled call reports that the store was not
  /// confirmed rather than that it did not happen.
  std::stop_token stop{};
};

/// @brief Retry/timing for the restore-default-parameters confirmation walk
///        (@c ProfileDevice::runRestoreDefaultParameters).
///
/// The restore counterpart of @c StoreParametersConfig, with the same fields and defaults: after
/// the "load" signature is written, wait @c settle for the device to begin, then poll the 0x1011
/// sub-entry up to @c retries more times, @c interval apart, until it reads back 1.
struct RestoreDefaultParametersConfig {
  uint32_t retries = 10;                    ///< Maximum confirmation polls after the first.
  std::chrono::milliseconds interval{500};  ///< Delay between confirmation polls.
  std::chrono::milliseconds settle{1000};   ///< Wait after the write before the first poll.
  std::stop_token stop{};                   ///< Abandons the wait, not the restore (see above).
};

/// @brief Root of the drive-profile view hierarchy — a concrete, instantiable, borrowed view over
///        a @c Device that exposes the generic CANopen device area (CiA301).
///
/// This is **not** an abstract interface: it is directly usable on its own. Every CoE device — not
/// just CiA402 drives — carries the generic device-profile objects (manufacturer device name and
/// software version, identity, store/restore, error register, ...), so those accessors live here,
/// on the root, and any device with an enumerated object dictionary can be viewed as a
/// @c ProfileDevice. @c Cia402Drive refines it with the drive state machine; @c SomanetDrive
/// refines that with the vendor object range.
///
/// A profile is not what a device *is* in storage — every device is a @c Device, value-stored in
/// @c DeviceManager. A profile is *what you do with* a device, using knowledge of its
/// object-dictionary layout. So profile types do not derive from @c Device nor are they owned by
/// it; they **borrow** a @c Device& and are constructed on demand for a single operation (a stack
/// local in an HTTP handler, or a member of a cyclic task scoped to that task's lifetime).
///
/// Because these views are never stored base-typed (no @c vector<ProfileDevice>, no polymorphic
/// container), the inheritance chain @c SomanetDrive → @c Cia402Drive → @c ProfileDevice is a
/// genuine is-a relationship with no slicing or downcasting hazard — the objection that makes
/// inheritance *on* @c Device wrong does not apply to a borrowed view.
///
/// **Invariant: a profile view holds no state beyond this reference.** A CiA402 drive's state
/// lives in its statusword on the wire, not in the view; multi-cycle procedure state lives in a
/// cyclic task, not here. The methods here are pure behaviour — thin typed reads/writes over
/// @c device() — so the day a view needs a persistent data member is the day this borrowed-view
/// model needs rethinking.
///
/// @warning The borrowed @c Device& must outlive the view. Construct a view, use it, and drop
///          it within a single operation; never cache one across a bus rescan (which rebuilds
///          @c DeviceManager's device vector and would dangle the reference). A cyclic task
///          re-resolves its @c Device via @c DeviceManager::findDevice every cycle for exactly
///          this reason.
class ProfileDevice {
 public:
  /// @brief Binds an (unchecked) view to @p device. Prefer @c createProfileDevice.
  explicit ProfileDevice(Device& device) : device_(device) {}

  /// @brief The underlying generic device this view operates on.
  Device& device() { return device_; }
  const Device& device() const { return device_; }

  /// @brief Reads the device type (0x1000, UNSIGNED32) — the device profile the object dictionary
  ///        implements (low word = profile number, e.g. 402; high word = profile-specific info).
  std::expected<uint32_t, std::string> deviceType() const;

  /// @brief Reads the error register (0x1001, UNSIGNED8) — the active error class bit field
  ///        (bit 0 = generic error). Read-only but volatile, so every call re-reads the device.
  std::expected<uint8_t, std::string> errorRegister() const;

  /// @brief Reads the COB-ID of the SYNC message (0x1005, INTEGER32). Writable, so every call
  ///        re-reads the device.
  std::expected<int32_t, std::string> cobIdSync() const;

  /// @brief Writes the COB-ID of the SYNC message (0x1005, INTEGER32).
  std::expected<void, std::string> setCobIdSync(int32_t value);

  /// @brief Reads the communication cycle period (0x1006, INTEGER32, µs). Writable, so every call
  ///        re-reads the device.
  std::expected<int32_t, std::string> communicationCyclePeriod() const;

  /// @brief Writes the communication cycle period (0x1006, INTEGER32, µs).
  std::expected<void, std::string> setCommunicationCyclePeriod(int32_t value);

  /// @brief Reads the manufacturer device name (0x1008, VISIBLE_STRING).
  ///
  /// A live re-read over the wire, distinct from @c Device::name() (the EtherCAT slave name cached
  /// at scan): this is the CoE object the firmware reports for itself.
  std::expected<std::string, std::string> manufacturerDeviceName() const;

  /// @brief Reads the manufacturer software version (0x100A, VISIBLE_STRING) — the firmware
  /// version.
  std::expected<std::string, std::string> manufacturerSoftwareVersion() const;

  /// @brief Reads the node guarding guard time (0x100C, UNSIGNED16, ms). Writable, so every call
  ///        re-reads the device.
  std::expected<uint16_t, std::string> guardTime() const;

  /// @brief Writes the node guarding guard time (0x100C, UNSIGNED16, ms).
  std::expected<void, std::string> setGuardTime(uint16_t value);

  /// @brief Reads the node guarding life time factor (0x100D, UNSIGNED8). Writable, so every call
  ///        re-reads the device.
  std::expected<uint8_t, std::string> lifeTimeFactor() const;

  /// @brief Writes the node guarding life time factor (0x100D, UNSIGNED8).
  std::expected<void, std::string> setLifeTimeFactor(uint8_t value);

  /// @brief Reads "save all parameters" (0x1010:01, UNSIGNED32) — the save capability (bit 0 =
  ///        device saves on command); writing the "save" signature to it triggers the store.
  ///        Writable, so every call re-reads the device.
  std::expected<uint32_t, std::string> storeParameters() const;

  /// @brief Writes "save all parameters" (0x1010:01, UNSIGNED32). Writing the ASCII "save"
  ///        signature (0x65766173) commands the device to store its parameters to non-volatile
  ///        memory; the device aborts any other value.
  std::expected<void, std::string> setStoreParameters(uint32_t signature);

  /// @brief Commands a parameter store (0x1010) and waits for the device to confirm it completed.
  ///
  /// The command-and-wait procedure built on the two raw accessors above: writes the ASCII "save"
  /// signature (@c setStoreParameters), waits @c config.settle for the device to begin the
  /// non-volatile write, then polls @c storeParameters (0x1010:01) until it reads back 1 — the
  /// CiA301 "save completed" value. A store can take a second or more (the drive persists its
  /// config to flash), and while it is in progress the mailbox may briefly refuse the read, so each
  /// poll that does not yet confirm — a value mismatch or a read error alike — is retried up to
  /// @c config.retries more times, @c config.interval apart, before the call gives up.
  ///
  /// Blocks the calling thread for up to @c settle plus @c retries × @c interval. Intended to run
  /// on the control-plane (HTTP) thread: it sleeps between polls but each poll's bus access takes
  /// the driver's control-plane lock only per transaction, so it never blocks the RT loop or the
  /// WebSocket. Requires the device's mailbox to be active (PRE-OP/SAFE-OP/OP).
  ///
  /// @param config  Retry/timing configuration (see @c StoreParametersConfig); the defaults suit a
  ///                normal store.
  /// @return Void once 0x1010:01 reads 1, or an error string if the store command write fails or
  /// the
  ///         device does not confirm within the retry budget.
  std::expected<void, std::string> runStoreParameters(const StoreParametersConfig& config = {});

  /// @brief Reads the restore-default-parameters object (0x1011) — the restore capability of all
  ///        four groups. Writable (the "load" signature triggers a restore), so every call
  ///        re-reads the device. Fails if any sub-entry read fails.
  std::expected<RestoreDefaultParameters, std::string> restoreDefaultParameters() const;

  /// @brief Writes "restore all default parameters" (0x1011:01, UNSIGNED32). Writing the ASCII
  ///        "load" signature (0x64616F6C) commands the restore; the device aborts any other value.
  std::expected<void, std::string> setRestoreAllDefaultParameters(uint32_t signature);

  /// @brief Writes "restore communication default parameters" (0x1011:02, UNSIGNED32) — the
  ///        ASCII "load" signature (0x64616F6C) commands the restore.
  std::expected<void, std::string> setRestoreCommunicationDefaultParameters(uint32_t signature);

  /// @brief Writes "restore application default parameters" (0x1011:03, UNSIGNED32) — the ASCII
  ///        "load" signature (0x64616F6C) commands the restore.
  std::expected<void, std::string> setRestoreApplicationDefaultParameters(uint32_t signature);

  /// @brief Writes "restore manufacturer-defined default parameters" (0x1011:04, UNSIGNED32) —
  ///        the ASCII "load" signature (0x64616F6C) commands the restore.
  std::expected<void, std::string> setRestoreManufacturerDefaultParameters(uint32_t signature);

  /// @brief Commands a restore of default parameters (0x1011) and waits for the device to confirm.
  ///
  /// The restore counterpart of @c runStoreParameters: writes the ASCII "load" signature
  /// (0x64616F6C) to the 0x1011 sub-entry selected by @p group, waits @c config.settle, then polls
  /// that sub-entry until it reads back 1 — the CiA301 "restore completed" value — retrying a poll
  /// that does not yet confirm (a value mismatch or a transient mailbox read error) up to
  /// @c config.retries more times, @c config.interval apart.
  ///
  /// **Destructive:** this overwrites the selected group's parameter values in the device's
  /// volatile memory with the device's defaults, replacing the current live values (not the
  /// persisted store). Which values it touches, and whether the change takes effect immediately or
  /// after the next reset, is device-specific. Blocks the calling thread and takes the driver's
  /// control-plane lock only per transaction, exactly like @c runStoreParameters; requires the
  /// mailbox to be active (PRE-OP/SAFE-OP/OP).
  ///
  /// @param group   Which group of defaults to restore (maps to the 0x1011 sub-entry).
  /// @param config  Retry/timing configuration (see @c RestoreDefaultParametersConfig).
  /// @return Void once the sub-entry reads 1, or an error string if the restore command write fails
  ///         (e.g. the device does not support @p group) or it is not confirmed within the budget.
  std::expected<void, std::string> runRestoreDefaultParameters(
      RestoreGroup group, const RestoreDefaultParametersConfig& config = {});

  /// @brief Reads the consumer heartbeat time (0x1016:01, UNSIGNED32, ms — node ID in bits 16-23).
  ///        Writable, so every call re-reads the device.
  std::expected<uint32_t, std::string> consumerHeartbeatTime() const;

  /// @brief Writes the consumer heartbeat time (0x1016:01, UNSIGNED32, ms — node ID in bits
  ///        16-23).
  std::expected<void, std::string> setConsumerHeartbeatTime(uint32_t value);

  /// @brief Reads the producer heartbeat time (0x1017, UNSIGNED16, ms). Writable, so every call
  ///        re-reads the device.
  std::expected<uint16_t, std::string> producerHeartbeatTime() const;

  /// @brief Writes the producer heartbeat time (0x1017, UNSIGNED16, ms).
  std::expected<void, std::string> setProducerHeartbeatTime(uint16_t value);

  /// @brief Reads the identity object (0x1018) — vendor ID, product code, revision, serial.
  ///
  /// A live re-read over the wire of all four sub-entries, distinct from the same values cached on
  /// @c Device at scan (@c Device::vendorId() etc.). Fails if any sub-entry read fails.
  std::expected<Identity, std::string> identity() const;

  /// @brief Reads the synchronous counter overflow value (0x1019, UNSIGNED8). Writable, so every
  ///        call re-reads the device.
  std::expected<uint8_t, std::string> synchronousCounterOverflowValue() const;

  /// @brief Writes the synchronous counter overflow value (0x1019, UNSIGNED8).
  std::expected<void, std::string> setSynchronousCounterOverflowValue(uint8_t value);

  /// @brief Reads back the OS command bytes (0x1023:01, 8 bytes). Writable (this is the command
  ///        the caller last issued), so every call re-reads the device.
  std::expected<std::vector<uint8_t>, std::string> osCommand() const;

  /// @brief Writes the OS command bytes (0x1023:01, 8 bytes) — issues an OS command; poll
  ///        @c osCommandStatus / @c osCommandResponse for the result.
  std::expected<void, std::string> setOsCommand(const std::vector<uint8_t>& command);

  /// @brief Reads the OS command status (0x1023:02, UNSIGNED8). Read-only but volatile (it tracks
  ///        the last issued command), so every call re-reads the device.
  std::expected<uint8_t, std::string> osCommandStatus() const;

  /// @brief Reads the OS command response bytes (0x1023:03, 8 bytes). Read-only but volatile (it
  ///        tracks the last issued command), so every call re-reads the device.
  std::expected<std::vector<uint8_t>, std::string> osCommandResponse() const;

  /// @brief Writes the OS command mode (0x1024, UNSIGNED8). Write-only on the device — there is
  ///        no matching getter.
  std::expected<void, std::string> setOsCommandMode(uint8_t mode);

 protected:
  /// @brief The borrowed device — the only data member permitted in the whole view chain.
  Device& device_;
};

/// @brief Binds a generic device-profile view to @p device.
///
/// Offline-safe discriminator: the device must expose the device type (0x1000) in its
/// (already-enumerated) object dictionary — a mandatory CANopen object present on any CoE device,
/// so this only guards against an un-enumerated device, not a wrong device class. No bus I/O is
/// performed.
///
/// @param device  Device to view. The reference must outlive the returned view.
/// @return A @c ProfileDevice bound to @p device, or an error string if its parameters have not
///         been initialised.
std::expected<ProfileDevice, std::string> createProfileDevice(Device& device);

}  // namespace mm::node
