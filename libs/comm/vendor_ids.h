#pragma once

#include <cstdint>

namespace mm::comm {

/// @brief Synapticon's EtherCAT Vendor ID (SII and object 0x1018:01).
///
/// In @c mm::comm because the fieldbus driver must know the vendor, and @c comm cannot depend on
/// @c node. @c mm::node::kSynapticonVendorId names this same constant.
inline constexpr uint32_t kSynapticonVendorId = 0x000022D2;

/// @brief Sensodrive's EtherCAT Vendor ID (SII and object 0x1018:01).
///
/// Sensodrive devices run SOMANET firmware, so @c isSomanetDevice is true for them.
inline constexpr uint32_t kSensodriveVendorId = 0x0000063A;

/// @brief Whether a device with this vendor ID is a SOMANET device.
///
/// A SOMANET device runs SOMANET firmware. The vendor ID is the evidence for that, and it comes
/// from the SII, so it is known as soon as the device is. Sensodrive devices run SOMANET firmware
/// unmodified under their own vendor ID.
constexpr bool isSomanetDevice(uint32_t vendorId) {
  return vendorId == kSynapticonVendorId || vendorId == kSensodriveVendorId;
}

}  // namespace mm::comm
