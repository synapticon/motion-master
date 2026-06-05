#pragma once

#include "node/device.h"

namespace mm::node {

/// @brief Base of the drive-profile view hierarchy: a thin, borrowed view over a @c Device.
///
/// A profile (CiA402 and, above it, SOMANET) is not what a device *is* in storage — every
/// device is a @c Device, value-stored in @c DeviceManager. A profile is *what you do with*
/// a device, using knowledge of its object-dictionary layout. So profile types do not derive
/// from @c Device nor are they owned by it; they **borrow** a @c Device& and are constructed
/// on demand for a single operation (a stack local in an HTTP handler, or a member of a cyclic
/// task scoped to that task's lifetime).
///
/// Because these views are never stored base-typed (no @c vector<ProfileDevice>, no polymorphic
/// container), the inheritance chain @c SomanetDrive → @c Cia402Drive → @c ProfileDevice is a
/// genuine is-a relationship with no slicing or downcasting hazard — the objection that makes
/// inheritance *on* @c Device wrong does not apply to a borrowed view.
///
/// **Invariant: a profile view holds no state beyond this reference.** A CiA402 drive's state
/// lives in its statusword on the wire, not in the view; multi-cycle procedure state lives in a
/// cyclic task, not here. Keep subclasses data-free except for behaviour — the day a view needs
/// a persistent data member is the day this borrowed-view model needs rethinking.
///
/// @warning The borrowed @c Device& must outlive the view. Construct a view, use it, and drop
///          it within a single operation; never cache one across a bus rescan (which rebuilds
///          @c DeviceManager's device vector and would dangle the reference). A cyclic task
///          re-resolves its @c Device via @c DeviceManager::findDevice every cycle for exactly
///          this reason.
class ProfileDevice {
 public:
  /// @brief Binds the view to @p device. The reference must outlive this view.
  explicit ProfileDevice(Device& device) : device_(device) {}

  /// @brief The underlying generic device this view operates on.
  Device& device() { return device_; }
  const Device& device() const { return device_; }

 protected:
  /// @brief The borrowed device — the only data member permitted in the whole view chain.
  Device& device_;
};

}  // namespace mm::node
