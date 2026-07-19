#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "node/device.h"

namespace mm::node {

/// @brief Generic CANopen device-profile (CiA301) object indices — present on **any** CoE device,
///        independent of the CiA402 drive profile. Subindex is 0 for these simple objects.
enum GenericObject : uint16_t {
  kDeviceType = 0x1000,                   ///< UNSIGNED32 — mandatory; the device profile it speaks.
  kManufacturerDeviceName = 0x1008,       ///< VISIBLE_STRING — human-readable device name.
  kManufacturerSoftwareVersion = 0x100A,  ///< VISIBLE_STRING — firmware/software version string.
  kIdentity = 0x1018,                     ///< IDENTITY record — vendor/product/revision/serial.
};

/// @brief The CANopen identity object (0x1018) — the four UNSIGNED32 sub-entries that uniquely
///        identify a device on the bus.
struct Identity {
  uint32_t vendorId{};        ///< 0x1018:01 — EtherCAT vendor ID.
  uint32_t productCode{};     ///< 0x1018:02 — vendor-assigned product code.
  uint32_t revisionNumber{};  ///< 0x1018:03 — revision number.
  uint32_t serialNumber{};    ///< 0x1018:04 — device serial number.
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

  /// @brief Reads the manufacturer device name (0x1008, VISIBLE_STRING).
  ///
  /// A live re-read over the wire, distinct from @c Device::name() (the EtherCAT slave name cached
  /// at scan): this is the CoE object the firmware reports for itself.
  std::expected<std::string, std::string> manufacturerDeviceName() const;

  /// @brief Reads the manufacturer software version (0x100A, VISIBLE_STRING) — the firmware
  /// version.
  std::expected<std::string, std::string> manufacturerSoftwareVersion() const;

  /// @brief Reads the identity object (0x1018) — vendor ID, product code, revision, serial.
  ///
  /// A live re-read over the wire of all four sub-entries, distinct from the same values cached on
  /// @c Device at scan (@c Device::vendorId() etc.). Fails if any sub-entry read fails.
  std::expected<Identity, std::string> identity() const;

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
