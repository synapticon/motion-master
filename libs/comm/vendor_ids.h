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
/// Sensodrive devices run SOMANET firmware, so firmware behaviour that the driver works around
/// applies to them too.
inline constexpr uint32_t kSensodriveVendorId = 0x0000063A;

}  // namespace mm::comm
