#pragma once

#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mm::node {

/// @file
/// @brief The @c .hardware_description file: what a SOMANET product says it is, and the descriptor
///        that decides which firmware fits it.
///
/// Pure transform over text — no fieldbus, no filesystem, no HTTP — so the rule that decides
/// whether a firmware package belongs on a device is unit-testable against real files without
/// hardware. Reading the file off a drive is @c SomanetDrive::readHardwareDescription; matching a
/// package against it is @c checkFirmwareCompatibility in @c firmware_package.h.
///
/// Every name here is the one the *Hardware description specification* (v1.5, format 1.2.0) uses,
/// including its §3.3 clarifying names — @c firmwareId for a product's @c id, @c firmwareVersion
/// for its @c version, @c buildDescriptor for the two joined, @c fullFirmwareDescriptor for the
/// whole thing. Source code elsewhere in the ecosystem calls some of these something else (the
/// descriptor was also called an "API identifier" and an "FWID"); those names are not used here.

/// @brief One item making up a device or assembly (specification §3.2.2.3).
///
/// Carries no function beyond recording what has a serial number: the specification says so
/// outright. Every field is optional there, so every one of these can legitimately be empty.
struct HardwareComponent {
  std::string name;          ///< Human-understandable description, e.g. "Com EtherCAT".
  std::string version;       ///< Version of the hardware named above, e.g. "B.4".
  std::string serialNumber;  ///< Unique identifier for this component.
};

/// @brief A device or an assembly — the two are one shape (specification §3.2.2).
///
/// The specification introduces both as concepts rather than as one type with two roles, and lists
/// their fields in a single table: they differ only in that @c keyId and @c macAddress are
/// device-only. One struct for both is what that table describes, and it is what makes
/// @c buildDescriptor a single function rather than two that must not drift apart.
///
/// "Product" is the specification's own word for what these describe — @c id "identifies the
/// hardware product" — which is why it names the type that has to cover both.
struct HardwareProduct {
  std::string name;          ///< Human-readable description of the hardware. Required.
  std::string imageId;       ///< Names the product's picture, e.g. "9501-xx". Optional.
  std::string id;            ///< firmwareId — identifies the hardware product, e.g. "9501".
  std::string version;       ///< firmwareVersion — revision of that product, e.g. "01".
  std::string keyId;         ///< Firmware encryption key burned to OTP, e.g. "4622". Device only.
  std::string serialNumber;  ///< Identifies this particular unit. Optional.
  std::string macAddress;    ///< MAC address of the Ethernet port. Device only, optional.
  std::vector<HardwareComponent> components;  ///< What this product is made of. Optional.

  /// @brief The @c buildDescriptor: @c id and @c version joined by a dash, e.g. "9501-01".
  ///
  /// Named for what the specification calls it in §3.3 (also written @c BUILD_DESCRIPTOR there),
  /// and deliberately not "versioned product id" or any of the other names this string has
  /// collected.
  std::string buildDescriptor() const;
};

/// @brief A parsed @c .hardware_description file (specification §3.2).
///
/// @c device is not optional and that is structural rather than a simplification: the file lives
/// *on* the device, so a file without one cannot exist. An assembly is present only when the device
/// was packaged into one — an ACTILINK is an assembly with a SOMANET Node device inside it — and
/// when it is, it is what firmware is matched against first.
struct HardwareDescription {
  std::string fileVersion;                  ///< Format version, semver, e.g. "1.2.0".
  HardwareProduct device;                   ///< The SOMANET device storing this file. Required.
  std::optional<HardwareProduct> assembly;  ///< The product it was built into, if any.
};

/// @brief Decodes @p content as a @c .hardware_description file.
///
/// Three things fail a parse: content that is not JSON, content with no @c device object, and a
/// device carrying no @c id or no @c version. Everything else is accepted as written, including a
/// missing @c fileVersion and missing optional fields.
///
/// **The @c fileVersion major is read but not enforced.** §3.2.1 says a major bump "reflects a
/// change in the existing names. A new parser is needed" — but refusing an unknown major would make
/// a drive flashed with a future file unreadable *today*, in exchange for guarding fields (@c id,
/// @c version, @c keyId) that a rename would be gratuitous to touch. The version is reported so a
/// caller that cares can decide; nothing here decides for it.
///
/// The three required fields are exactly what a descriptor is built from, which is also what makes
/// this a usable type check: arbitrary JSON does not carry a @c device with an @c id and a
/// @c version, so a file that is not a hardware description is rejected rather than silently
/// yielding an empty descriptor that matches no package.
///
/// @param content  The file's bytes as text; UTF-8 per §3.1.
/// @return The parsed description, or what made @p content unusable.
std::expected<HardwareDescription, std::string> parseHardwareDescription(std::string_view content);

/// @brief Serialises a component, a product and a whole description.
///
/// Field names are the file's own, so a serialised description reads as the file it came from, with
/// @c buildDescriptor added to each product because it is the part a client wants and would
/// otherwise re-derive. Empty optional strings are still emitted — the shape of a product should
/// not change with whether a serial number was recorded.
/// @{
void to_json(nlohmann::json& j, const HardwareComponent& component);
void to_json(nlohmann::json& j, const HardwareProduct& product);
void to_json(nlohmann::json& j, const HardwareDescription& description);
/// @}

}  // namespace mm::node
