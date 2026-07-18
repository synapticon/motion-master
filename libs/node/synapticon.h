#pragma once

#include <cstdint>
#include <string_view>

namespace mm::node {

/// @brief Synapticon's EtherCAT Vendor ID (object 0x1018:01) — the discriminator for a SOMANET
///        drive and the single source of truth for the vendor check.
///
/// Kept in its own tiny header so both the profile-view chain (@c SomanetDrive) and the
/// object-dictionary cache (@c ParameterCache) can share one definition without either depending
/// on the other's headers.
inline constexpr uint32_t kSynapticonVendorId = 0x000022D2;

/// @brief Known SOMANET product codes (object 0x1018:02) under @c kSynapticonVendorId.
///
/// The product code together with the revision number keys the object-dictionary cache
/// (@c ParameterCache). This enumerates the products we recognise; a device may report a code not
/// listed here (a newer or unreleased product) — that is not an error, it simply has no name.
enum class SomanetProduct : uint32_t {
  kNode = 0x00000201,        ///< SOMANET Node.
  kCirculo = 0x00000301,     ///< SOMANET Circulo.
  kCirculoSmm = 0x00000302,  ///< SOMANET Circulo SMM.
  kIntegro = 0x00000401,     ///< SOMANET Integro.
};

/// @brief Human-readable name of a known SOMANET product (for logging / JSON). Returns "Unknown"
///        for a product code not in @c SomanetProduct.
///
/// The strings match the device @c <Type> name in Synapticon's ESI (EtherCAT Slave Information),
/// where "SOMANET" is part of the product name.
constexpr std::string_view toString(SomanetProduct product) {
  switch (product) {
    case SomanetProduct::kNode:
      return "SOMANET Node";
    case SomanetProduct::kCirculo:
      return "SOMANET Circulo";
    case SomanetProduct::kCirculoSmm:
      return "SOMANET Circulo SMM";
    case SomanetProduct::kIntegro:
      return "SOMANET Integro";
  }
  return "Unknown";
}

}  // namespace mm::node
