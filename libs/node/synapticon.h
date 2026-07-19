#pragma once

#include <cstdint>
#include <string_view>

namespace mm::node {

/// @brief Synapticon's EtherCAT Vendor ID (object 0x1018:01) — the discriminator for a SOMANET
///        drive and the single source of truth for the vendor check.
///
/// Kept in its own header so both the profile-view chain (@c SomanetDrive) and the
/// object-dictionary cache (@c ParameterCache) can share this definition without either depending
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

/// @brief Product name for a raw product code (object 0x1018:02), or empty if the code is not a
///        recognised SOMANET product.
///
/// Convenience over @c SomanetProduct + @c toString for callers holding a raw code (e.g.
/// @c Device::productCode()); the empty result lets a caller fall back to another name. The caller
/// is responsible for checking the vendor is @c kSynapticonVendorId — product codes are only unique
/// within a vendor.
constexpr std::string_view somanetProductName(uint32_t productCode) {
  switch (static_cast<SomanetProduct>(productCode)) {
    case SomanetProduct::kNode:
    case SomanetProduct::kCirculo:
    case SomanetProduct::kCirculoSmm:
    case SomanetProduct::kIntegro:
      return toString(static_cast<SomanetProduct>(productCode));
  }
  return {};
}

}  // namespace mm::node
