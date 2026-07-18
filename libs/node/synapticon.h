#pragma once

#include <cstdint>

namespace mm::node {

/// @brief Synapticon's EtherCAT Vendor ID (object 0x1018:01) — the discriminator for a SOMANET
///        drive and the single source of truth for the vendor check.
///
/// Kept in its own tiny header so both the profile-view chain (@c SomanetDrive) and the
/// object-dictionary cache (@c ParameterCache) can share one definition without either depending
/// on the other's headers.
inline constexpr uint32_t kSynapticonVendorId = 0x000022D2;

}  // namespace mm::node
