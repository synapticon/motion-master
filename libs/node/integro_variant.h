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
/// @brief The @c .variant file: which features a SOMANET Integro was licensed with, and the
///        fieldbus character its firmware descriptor carries.
///
/// Pure transform over bytes — no fieldbus, no filesystem, no HTTP. It exists here for one concrete
/// reason: the *Hardware description specification* §3.4.2.1 notes that the fieldbus protocol
/// **does not appear in the hardware description** and lives in this file instead, so a
/// full firmware descriptor ending in a fieldbus character cannot be assembled without reading it.
/// Everything else it decodes is a bonus that comes free with the header.
///
/// The layout is taken from the firmware that writes and reads it (@c App_Utils.h /
/// @c AppUtil_GetVariantData in @c somanet_software), not from a specification — there is none —
/// and verified against real files. A 128-byte header:
///
/// | Offset | Size | Field                                             |
/// | -----: | ---: | ------------------------------------------------- |
/// |      0 |    4 | file version                                      |
/// |      4 |   64 | signature                                         |
/// |     68 |   12 | netX chip id                                      |
/// |     80 |   24 | device serial number, NUL-padded                  |
/// |    104 |    8 | MAC address — 2 bytes of padding, then the 6 bytes |
/// |    112 |    4 | customer id                                       |
/// |    116 |    2 | operation mode                                    |
/// |    118 |   10 | reserved                                          |
///
/// followed by the variant data section: two reserved bytes, a two-byte option count, then that
/// many two-byte option codes. All little-endian and packed.
///
/// Named for the Integro because the option *catalogue* is: the firmware's own comment beside it
/// reads "These are option list for Integro. Other devices (even newer Integros) might have a
/// different options list in the future". The file layout itself is not Integro-specific, so a
/// device of another kind carrying one would still decode — only its option meanings would be a
/// guess, which is why an unknown code is reported as unknown rather than dropped.

/// @brief How the drive treats its variant options (firmware @c OPTIONS_MODE_*).
///
/// @c kPassive is the failure state as much as a mode: firmware falls back to it when the file is
/// missing, its signature does not verify, or its chip id belongs to another device — so a drive
/// reporting passive is one whose licensing did not take.
enum class VariantOperationMode : uint16_t {
  kPassive = 0x51,
  kTrial = 0x53,
  kProduction = 0x55,
  kLive = 0x57,
};

/// @brief The mode's name, or "unknown" for a value no firmware release defines.
std::string_view toString(VariantOperationMode mode);

/// @brief One entry of the Integro runtime-variant option catalogue.
///
/// The catalogue is data with no behaviour attached: nothing here acts on @c socVariables or
/// @c incompatibleOptionIds — the drive is what enforces them — but they are what make a decoded
/// file legible, so they travel with the entry rather than being looked up somewhere else.
struct IntegroVariantOption {
  uint16_t id = 0;            ///< The code as it appears in the file.
  std::string_view category;  ///< What the option selects, e.g. "Fieldbus Protocol".
  std::string_view meaning;   ///< Which choice within that category, e.g. "EtherCAT".

  /// The drive's own configuration values this option sets, e.g. @c adc_phase_current_sensor_gain.
  /// Empty for an option the fieldbus firmware acts on rather than the SoC.
  std::span<const std::string_view> socVariables;

  /// The other options in this option's category, where the category selects exactly one of them.
  /// Empty for an independent flag such as "disable DI1".
  ///
  /// A file selects one option per category. A file that selects two from the same category is a
  /// development device, set up so one unit can be tested in several configurations.
  std::span<const uint16_t> incompatibleOptionIds;

  /// Segments of the part number this option corresponds to, e.g. @c EC for EtherCAT. Empty for an
  /// option that is not distinguished in the part number.
  std::span<const std::string_view> mpnSegmentCodes;
};

/// @brief The whole catalogue, in code order.
///
/// Serving it is the point: a client that wants to explain a decoded file, or simply show what an
/// Integro can be licensed with, should not carry a second copy of this table.
///
/// **Maintained by hand.** It is vendor data copied into the master, so a firmware release that
/// adds an option code leaves it stale until someone adds the row. That is a deliberate choice
/// rather than an oversight: the catalogue's richest form — categories, SoC variables, part-number
/// segments and mutual exclusions — exists only in the commissioning tooling's table, and nothing
/// generatable carries it (the firmware's enum has ids and names alone, and the ESI does not
/// describe variants at all). It fails softly, which is what makes the choice affordable: an
/// unrecognised code still decodes and is still reported, as an id with no category or meaning, so
/// a file from a newer firmware lists everything it selects rather than dropping what this build
/// cannot name.
std::span<const IntegroVariantOption> integroVariantOptions();

/// @brief Looks a code up in the catalogue.
///
/// The catalogue comes from the commissioning tooling's table, which is a superset of the
/// firmware's own @c RuntimeVariantOptions enum — the enum does not list codes 21-26 or 39. The
/// superset is what an older file needs to decode.
///
/// @param id  The option code.
/// @return The catalogue entry, or @c nullptr for a code the catalogue does not name.
const IntegroVariantOption* integroVariantOption(uint16_t id);

/// @brief Serialises one catalogue entry — the body of @c GET @c /api/integro-variant/options.
void to_json(nlohmann::json& j, const IntegroVariantOption& option);

/// @brief A parsed @c .variant file.
///
/// @c signature is kept although nothing here can check it — verification needs the private key and
/// happens on the drive — so that a caller can at least see whether one is present.
struct IntegroVariant {
  uint32_t fileVersion = 0;             ///< Format version; 1 in every file seen so far.
  std::array<uint8_t, 64> signature{};  ///< Opaque; verified by the drive, never here.
  std::array<uint8_t, 12> chipId{};     ///< netX chip id this file was issued for.
  std::string serialNumber;             ///< The Integro's serial number, e.g. "9004-02-...".
  std::string macAddress;               ///< Colon-separated, e.g. "40:49:8A:01:21:D6".
  uint32_t customerId = 0;              ///< Who the device was licensed to.
  uint16_t operationMode = 0;           ///< Raw @c VariantOperationMode value.
  std::vector<uint16_t> optionIds;      ///< Option codes, in the order the file lists them.
};

/// @brief The most options a file may carry (firmware @c MAX_VARIANTS_COUNT).
///
/// Enforced rather than documented: firmware refuses a file claiming more with
/// @c OPTIONS_STATUS_TOO_MANY_OPTIONS, so a count above this is a file the drive itself would not
/// accept.
inline constexpr uint16_t kMaxVariantOptions = 32;

/// @brief Decodes @p content as a @c .variant file.
///
/// The option codes are left in file order rather than sorted. They arrive unsorted — a real file
/// reads `1, 17, 34, 4, 7, 10, 14` — and the order is the only record of how the file was written,
/// so sorting is left to whoever displays them.
///
/// Trailing bytes past the last option are accepted. Every file seen ends exactly at
/// `132 + 2 × count`, but the firmware reads only as many options as the count claims and ignores
/// the rest, so refusing padding would reject a file the drive is happy with.
///
/// @param content  The file's bytes.
/// @return The decoded file, or why @p content is not one.
std::expected<IntegroVariant, std::string> parseIntegroVariant(std::span<const uint8_t> content);

/// @brief The selected fieldbus protocol, for the tail of a full firmware descriptor
///        (specification §3.4.2.1).
///
/// One fieldbus is selected on a production device. A development device may have several selected,
/// so that one unit can be tested on more than one protocol; EtherCAT is the one to use when it is
/// among them.
///
/// Only Integro devices reach this. Node and Circulo are EtherCAT-only with every feature enabled,
/// and carry no variant file.
///
/// @param variant  A parsed file.
/// @return The code, or nothing when the file selects no fieldbus.
std::optional<uint16_t> variantFieldbusProtocol(const IntegroVariant& variant);

/// @brief Serialises a parsed file.
///
/// @c signature and @c chipId become lowercase hex strings rather than arrays of numbers, the
/// operation mode carries its decoded name beside its value, and each option code is expanded into
/// its catalogue entry — with the entry's fields absent for a code the catalogue does not name, so
/// "we do not know what this is" is missing keys rather than the string "unknown".
void to_json(nlohmann::json& j, const IntegroVariant& variant);

}  // namespace mm::node
