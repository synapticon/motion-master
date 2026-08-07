#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mm::node {

/// @file
/// @brief What a SOMANET firmware package is, offline: how its filename decodes and what its zip
///        holds.
///
/// Pure transform over bytes and text — no fieldbus, no filesystem, no HTTP — so the rules that
/// decide what gets written to a drive are unit-testable against real packages without hardware.
/// The installing half is the firmware installation procedure in @c somanet_procedures.h.

/// @brief The entries an install skips unless the request names a different set.
///
/// Both are descriptive extras rather than firmware: @c SOMANET_CiA_402.xml.zip is the ESI, which a
/// commissioning tool reads from the package itself rather than from the drive, and
/// @c stack_image.svg.zip is a picture of the hardware. Writing them costs a slow FoE transfer each
/// (the ESI alone takes ~17 s to erase before it can be rewritten) and consumes drive flash to
/// store something nothing on the drive reads. They are skipped by default and un-skippable only
/// deliberately, by sending a request that omits them from the list.
inline constexpr std::array<std::string_view, 2> kDefaultSkippedFirmwareFiles = {
    "SOMANET_CiA_402.xml.zip",
    "stack_image.svg.zip",
};

/// @brief A SOMANET firmware package filename, broken into the five fields the naming convention
///        defines (Hardware description specification §3.4.2).
///
/// The grammar is @c
/// package_<hardware-name>_<fullFirmwareDescriptor>_<software-name>_v<version>.zip — five
/// underscore-separated fields, dashes standing in for spaces within a field. For example
/// @c package_SOMANET-Circulo-7_8500-04-2332_motion-drive_v5.6.10.zip.
struct FirmwarePackageName {
  std::string description;      ///< Always "package" for a firmware bundle.
  std::string hardwareName;     ///< Human-readable hardware name, e.g. "SOMANET-Circulo-7".
  std::string firmwareId;       ///< Full firmware descriptor, e.g. "8500-04-2332".
  std::string firmwareName;     ///< Software name, e.g. "motion-drive".
  std::string firmwareVersion;  ///< Software version including the leading 'v', e.g. "v5.6.10".

  /// @name Decoded firmware descriptor
  /// Present only when @c firmwareId follows the numeric `<id>-<version>[-<key>[-<fieldbus>]]`
  /// convention. The specification explicitly allows a descriptor that does not — its own example
  /// @c Branded-Drive-Elite carries @c MyProduct-v25-key3-ecat — so these are an optional bonus for
  /// display, never something the install depends on.
  /// @{
  std::optional<uint32_t> productId;         ///< e.g. 8500.
  std::optional<uint32_t> productVersion;    ///< e.g. 4.
  std::optional<uint32_t> keyId;             ///< Firmware encryption key id, e.g. 2332.
  std::optional<uint32_t> fieldbusProtocol;  ///< Hexadecimal; 1 = EtherCAT.
  /// @}
};

/// @brief Decodes @p filename as a SOMANET firmware package name.
///
/// A Windows duplicate-download suffix (@c " (1)" before the extension) is removed first, because a
/// user who downloaded the same package twice should not be told their file is unrecognised.
///
/// Whether a name parses matters for exactly one thing: a package is only cached under a name that
/// decodes, so the cache cannot fill with files named anything at all. A package whose name does
/// not parse still installs — the bytes are what get written, and the name is metadata.
///
/// @param filename  Bare filename, with no directory part.
/// @return The decoded fields, or a message saying which part of the grammar @p filename missed.
std::expected<FirmwarePackageName, std::string> parseFirmwarePackageName(std::string_view filename);

/// @brief Serialises a decoded package name — the body of @c GET @c /api/firmware-package-name.
///
/// The four decoded-descriptor fields are omitted rather than sent as null when the descriptor is
/// not the numeric kind, so "this name carries no product id" is an absent key rather than a value
/// a client has to null-check.
void to_json(nlohmann::json& j, const FirmwarePackageName& name);

/// @brief One entry of a package, extracted.
struct FirmwarePackageFile {
  std::string name;              ///< Entry name as it appears in the zip.
  std::vector<uint8_t> content;  ///< Decompressed bytes.
};

/// @brief A firmware package's contents, sorted by where each entry is going.
///
/// The classification is by filename convention, which is what the drive's own bootloader expects:
/// @c app_*.bin is the application firmware, @c com_*.bin the communication (netX) firmware,
/// @c *.sii an EEPROM image, and anything else an ordinary file stored on the drive's flash.
struct FirmwarePackage {
  std::optional<FirmwarePackageFile> appBinary;  ///< @c app_*.bin — flashed in BOOT over FoE.
  std::optional<FirmwarePackageFile> comBinary;  ///< @c com_*.bin — flashed in BOOT over FoE.
  std::optional<FirmwarePackageFile> sii;        ///< @c *.sii — written to the ESC EEPROM.
  std::vector<FirmwarePackageFile> extras;       ///< Everything else — stored on flash as-is.
  std::vector<std::string> skipped;              ///< Entry names the skip list removed, in order.
};

/// @brief Reads a firmware package zip held in memory, dropping the entries named in @p skipFiles.
///
/// Skipping happens during extraction rather than after it, so a skipped entry is never
/// decompressed — which is most of the point, since the two default-skipped files are the largest
/// in a typical package.
///
/// One content check is made here rather than left to the caller: a zip with no @c app_*.bin entry
/// at all is rejected, because that is not a firmware package and saying so before a device is
/// taken to BOOT is far better than discovering it afterwards. An @c app_*.bin that is present but
/// *skipped* is fine — that is a deliberate request to install only the rest.
///
/// @param zip        Package bytes.
/// @param skipFiles  Entry names to leave out, compared exactly.
/// @return The classified contents, or a message describing what made the zip unreadable.
std::expected<FirmwarePackage, std::string> openFirmwarePackage(
    std::span<const uint8_t> zip, std::span<const std::string> skipFiles);

}  // namespace mm::node
