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

#include "node/hardware_description.h"
#include "node/integro_variant.h"

namespace mm::node {

/// @file
/// @brief What a SOMANET firmware package is, offline: how its filename decodes, what its zip
/// holds,
///        and which hardware it belongs on.
///
/// Pure transform over bytes and text — no fieldbus, no filesystem, no HTTP — so the rules that
/// decide what gets written to a drive are unit-testable against real packages without hardware.
/// The installing half is the firmware installation procedure in @c somanet_procedures.h.
///
/// Names follow the *Hardware description specification* §3.3, which is worth reading once because
/// two of its terms are easy to swap: a package's @c fullFirmwareDescriptor is the whole
/// `<firmwareId>-<firmwareVersion>[-<keyId>[-<fieldbusProtocol>]]` string, while @c firmwareVersion
/// alone is the *hardware* revision inside it ("04") and has nothing to do with the software
/// version the filename ends with ("v5.6.10"). Hence @c softwareName / @c softwareVersion for that
/// pair.

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
  std::string description;             ///< Always "package" for a firmware bundle.
  std::string hardwareName;            ///< Human-readable hardware name, e.g. "SOMANET-Circulo-7".
  std::string fullFirmwareDescriptor;  ///< The hardware this is built for, e.g. "8500-04-2332".
  std::string softwareName;            ///< Firmware or software name, e.g. "motion-drive".
  std::string softwareVersion;         ///< Its version including the leading 'v', e.g. "v5.6.10".

  /// @name Decoded full firmware descriptor
  /// Present only when @c fullFirmwareDescriptor follows the numeric
  /// `<firmwareId>-<firmwareVersion>[-<keyId>[-<fieldbusProtocol>]]` convention. The specification
  /// explicitly allows a descriptor that does not — its own example @c Branded-Drive-Elite carries
  /// @c MyProduct-v25-key3-ecat — so these are an optional bonus for display, never something the
  /// install or a compatibility check depends on: matching is done on the whole descriptor string.
  ///
  /// Kept as text rather than numbers so a descriptor survives being decoded and shown: a
  /// @c firmwareVersion of "04" keeps its leading zero, and a @c keyId of "A" (which real packages
  /// carry) stays "A" rather than becoming a failed number.
  /// @{
  std::optional<std::string> firmwareId;        ///< e.g. "8500".
  std::optional<std::string> firmwareVersion;   ///< Hardware revision, e.g. "04".
  std::optional<std::string> keyId;             ///< Firmware encryption key id, e.g. "2332".
  std::optional<std::string> fieldbusProtocol;  ///< Hexadecimal character; "1" = EtherCAT.
  /// @}

  /// @brief The @c buildDescriptor — @c firmwareId and @c firmwareVersion joined, e.g. "8500-04".
  ///
  /// Nothing when the descriptor is not the numeric kind. Useful for saying *how* a package missed:
  /// a matching build descriptor with a different key means right hardware, wrong encryption key.
  std::optional<std::string> buildDescriptor() const;
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

/// @brief The full firmware descriptors one device accepts firmware under (specification §3.4.1).
///
/// Two rather than one, because the specification says both are compatible and lists both for the
/// user to choose from (§4.1 step 5). The distinction is worth keeping: assembly firmware "should
/// have been customized for the assembly", so it is the one to prefer, while the device package is
/// the generic build for the hardware inside it.
struct FullFirmwareDescriptors {
  /// The assembly's build descriptor with the **device's** key id, e.g. "6000-01-2423-1". Nothing
  /// when the hardware description carries no assembly. Assemblies have no key of their own —
  /// §3.4.1 is explicit that the descriptor "must pull the ID and version from the assembly, the
  /// keyId from the device".
  std::optional<std::string> assembly;

  /// The device's own descriptor, e.g. "9010-02-2423-1". Always present: a hardware description
  /// always describes a device.
  std::string device;
};

/// @brief Assembles the descriptors @p description accepts, appending @p variant's fieldbus
///        character when there is one.
///
/// The fieldbus tail comes from the @c .variant file and nowhere else — §3.4.2.1 notes it "does not
/// appear in the hardware description" — so a device without one (any drive that is not an Integro)
/// yields descriptors of three fields, which is exactly what its packages are named with. Passing a
/// null @p variant is therefore normal rather than a degraded mode.
///
/// @param description  A parsed hardware description.
/// @param variant      The device's parsed @c .variant, or @c nullptr when it has none.
/// @return Both descriptors, ready to compare against a package name.
FullFirmwareDescriptors fullFirmwareDescriptors(const HardwareDescription& description,
                                                const IntegroVariant* variant);

/// @brief Which of a device's descriptors a package matched.
enum class FirmwareMatch {
  kNone,      ///< Neither — the package is for other hardware.
  kAssembly,  ///< The assembly descriptor: firmware customised for the assembled product.
  kDevice,    ///< The device descriptor: the generic build for the hardware itself.
};

/// @brief What a match is called, for a message or a JSON body: "none", "assembly", "device".
std::string_view toString(FirmwareMatch match);

/// @brief The verdict on one package against one device.
struct FirmwareCompatibility {
  FirmwareMatch match = FirmwareMatch::kNone;  ///< What matched, if anything.
  FirmwarePackageName packageName;             ///< The package's decoded filename.
  FullFirmwareDescriptors deviceDescriptors;   ///< What the device accepts.

  /// One sentence naming the descriptors involved, ready to show a user. Populated for a match as
  /// well as a mismatch, because "compatible, but this is the generic device package rather than
  /// the one built for its assembly" is worth saying.
  std::string explanation;

  bool compatible() const { return match != FirmwareMatch::kNone; }
};

/// @brief Decides whether @p packageName belongs on the hardware @p description describes.
///
/// **The whole descriptor string is compared, never its decoded parts.** That is what makes the
/// check correct for descriptors the numeric convention does not cover, and it is how the
/// specification frames the operation: "Finding compatible firmware inside a directory with many
/// files can be done by searching the directory for the @c fullFirmwareDescriptor string" (§3.4.2).
///
/// Returns a value rather than an @c expected because with both inputs already parsed there is
/// nothing left to fail: a package that does not fit is a *verdict*, and the caller wants both
/// descriptors so it can say which hardware the package was for.
///
/// @param description  A parsed hardware description.
/// @param packageName  A parsed package filename.
/// @param variant      The device's parsed @c .variant, or @c nullptr when it has none.
/// @return The verdict.
FirmwareCompatibility checkFirmwareCompatibility(const HardwareDescription& description,
                                                 const FirmwarePackageName& packageName,
                                                 const IntegroVariant* variant = nullptr);

/// @brief Parses both inputs, then decides — the whole check from a file's text and a filename.
///
/// The entry point for a caller holding raw inputs (a device's file, a user's chosen filename). The
/// failure shape differs from the overload above on purpose: inputs that cannot be read at all — a
/// filename that is not a package name, content that is not a hardware description — are an
/// @c unexpected, because there is no verdict to give and the reason is what a caller shows
/// instead.
///
/// @param hardwareDescriptionContent  The device's @c .hardware_description, as text.
/// @param packageFilename             Bare package filename, with no directory part.
/// @param variant                     The device's parsed @c .variant, or @c nullptr when it has
///                                    none.
/// @return The verdict, or why no verdict could be reached.
std::expected<FirmwareCompatibility, std::string> checkFirmwareCompatibility(
    std::string_view hardwareDescriptionContent, std::string_view packageFilename,
    const IntegroVariant* variant = nullptr);

/// @brief Serialises the descriptors and a verdict.
///
/// @c assembly is omitted rather than null when the device is not part of one, matching how the
/// hardware description itself is serialised.
/// @{
void to_json(nlohmann::json& j, const FullFirmwareDescriptors& descriptors);
void to_json(nlohmann::json& j, const FirmwareCompatibility& compatibility);
/// @}

}  // namespace mm::node
