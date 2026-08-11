#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace mm {

/// @brief What one run of the object-address generator produced.
struct ObjectAddressGeneratorSummary {
  std::size_t rows = 0;               ///< Addresses emitted across every file.
  std::vector<std::string> files;     ///< Paths written, in the order they were written.
  std::vector<std::string> warnings;  ///< ESI parse warnings, plus anything the generator noticed.
};

/// @brief Builds the C++ identifier for one dictionary entry.
///
/// @c k + PascalCase(object name), plus PascalCase(entry name) when a subindex names something
/// different from its object. Subindex 0 of a composite is the entry-count field rather than a
/// value, so it becomes @c k<Object>Count — without that, the safety objects whose subindex 1
/// repeats the object's own name would collide with their own count field, which is six of the
/// SOMANET dictionary's 826 rows.
///
/// Punctuation and spaces are dropped, and a leading digit is prefixed, so the result is always a
/// valid identifier.
///
/// @param objectName  The object's name.
/// @param entryName   This subindex's name.
/// @param subindex    CoE subindex.
/// @param composite   Whether the object has subindices beyond 0 (ARRAY or RECORD).
std::string objectIdentifier(std::string_view objectName, std::string_view entryName,
                             uint8_t subindex, bool composite);

/// @brief Generates the object-address headers from a vendor's ESI file.
///
/// Parses @p esiPath and merges every device's dictionary — the profile dictionary plus each
/// module's, by the ETG.2000 rules @c mm::etg implements — then writes one header per index range
/// into @p outDir: @c profile_device_objects.h (0x1xxx plus the standard MDP objects),
/// @c cia402_drive_objects.h (0x6xxx), and @c somanet_drive_objects.h (everything else).
///
/// **The union of every device in the file is emitted, deliberately.** Which module is fitted is
/// unknowable offline, so the honest offering is every object the family could expose; an address
/// that does not apply to the drive in front of you simply fails to resolve at runtime, exactly as
/// a hand-written index would.
///
/// **One header per index range rather than per device rests on the vendor giving one address one
/// meaning.** Merging four devices into a single table keyed by @c (index,subindex) is sound only
/// if an address names the same quantity, with the same data type and unit, on every one of them.
/// The standards guarantee that for the communication area (0x1xxx) and the CiA 402 profile
/// (0x6xxx); for the manufacturer-specific area (0x2xxx) nothing does, so it is a SOMANET
/// convention — and it holds because the family's devices draw the bulk of their dictionary from
/// one shared ESI module rather than from four descriptions that happen to agree. Type
/// disagreements between devices are reported as warnings and resolved first-wins; one index reused
/// for a different quantity of the *same* type is what this cannot detect, and would mean a header
/// per device instead of one.
///
/// The generator itself knows nothing about types: @c mm::etg resolves each entry to the C++ type
/// it holds (@c EsiEntry::valueKind), including the width cross-check that keeps an
/// @c "ARRAY [0..24] OF BYTE" from being emitted as a @c uint8_t.
///
/// @param esiPath  Path to the vendor's ESI XML.
/// @param outDir   Directory to write the headers into; must exist.
/// @return A summary, or an error string if the ESI cannot be parsed or a file cannot be written.
std::expected<ObjectAddressGeneratorSummary, std::string> generateObjectAddresses(
    const std::string& esiPath, const std::string& outDir);

}  // namespace mm
