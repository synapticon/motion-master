#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string_view>

namespace mm::node::somanet {

/// @file
/// @brief The register map of the Integro's internal (Kübler) encoder — reference data for OS
///        command 19, and documentation in its own right.
///
/// Pure data with no dependency on @c Device, like @c cia402.h: a client picks a register from here
/// and the command reads or writes it. Served as-is by @c GET @c /api/meta/kuebler-registers.
///
/// **Transcribed from the vendor's own draft table** (`KueblerEncoderRegisters.csv`, headed "MagPac
/// Registecommunication draft"), and curated rather than converted, because two of its columns do
/// not survive a mechanical reading. Signedness is stated in free prose: register 0x38 Velocity is
/// signed and says so only as "+/- FS", while every POA register writes "Bit 15 - 0: signed 16 bit
/// integer", which is one clause spanning the whole register — a scalar, not a bit field. And 0x3C
/// is two *separate* signed 16-bit values in one 32-bit register, which no single signedness flag
/// describes. Hence @c KueblerFormat rather than a bool.

/// @brief How a register's value should be read once assembled.
enum class KueblerFormat : uint8_t {
  kUnsigned,  ///< One unsigned integer of the register's width.
  kSigned,    ///< One two's-complement integer of the register's width.
  kBitField,  ///< Named bits; the assembled number means nothing on its own. See @c definition.
  /// Two signed 16-bit values packed into 32 bits — the high half is A, the low half B. Register
  /// 0x3C is the only one, and decoding it as a single signed 32-bit value would be meaningless.
  kSignedHalves,
};

/// @brief Whether the encoder allows reading a register, writing it, or both.
enum class KueblerAccess : uint8_t { kReadOnly, kWriteOnly, kReadWrite };

/// @brief One register of the Kübler encoder.
struct KueblerRegister {
  uint8_t address = 0;    ///< Register address, as OS command 19 carries it in byte 2.
  uint8_t bits = 0;       ///< Width in bits: 8, 16, 32 — or 64, which the command cannot reach.
  std::string_view name;  ///< The vendor's own name for it.
  KueblerAccess access{KueblerAccess::kReadOnly};

  /// Whether the encoder implements it. **Seven of these are documented but not implemented**, and
  /// the draft says so per row; a client should mark them rather than offer them as working.
  bool implemented = false;

  KueblerFormat format{KueblerFormat::kUnsigned};

  /// The vendor's bit definitions, clauses joined with "; ". The only place a bit field's meaning
  /// is recorded, so it is carried verbatim rather than summarised.
  std::string_view definition;
};

/// @brief Serialises a @c KueblerRegister. Participates in nlohmann ADL, so
///        @c nlohmann::json(kKueblerRegisters) works.
void to_json(nlohmann::json& j, const KueblerRegister& r);

/// @brief Every register the vendor's draft documents, ascending by address.
inline constexpr auto kKueblerRegisters = std::to_array<KueblerRegister>({
    {0x00, 32, "Identification pattern", KueblerAccess::kReadOnly, false, KueblerFormat::kUnsigned,
     "TBD"},
    {0x04, 64, "Firmware-version", KueblerAccess::kReadOnly, false, KueblerFormat::kUnsigned,
     "Major(3 digit).Minor(3 digit).Timestamp(12 digits); 001.002.2023.09.18.14.25 --> Version "
     "1.2.202309181425"},
    {0x10, 16, "POA start offset A", KueblerAccess::kReadOnly, true, KueblerFormat::kSigned,
     "Bit 15 - 0: signed 16 bit integer"},
    {0x12, 16, "POA start offset B", KueblerAccess::kReadOnly, true, KueblerFormat::kSigned,
     "Bit 15 - 0: signed 16 bit integer"},
    {0x14, 16, "POA start amplitude A", KueblerAccess::kReadOnly, true, KueblerFormat::kUnsigned,
     "Bit 15 - 0: unsigned 16 bit integer"},
    {0x16, 16, "POA start amplitude B", KueblerAccess::kReadOnly, true, KueblerFormat::kUnsigned,
     "Bit 15 - 0: unsigned 16 bit integer"},
    {0x18, 16, "POA start phase", KueblerAccess::kReadOnly, true, KueblerFormat::kSigned,
     "Bit 15 - 0: signed 16 bit integer"},
    {0x1A, 16, "POA current offset A", KueblerAccess::kReadOnly, true, KueblerFormat::kSigned,
     "Bit 15 - 0: signed 16 bit integer"},
    {0x1C, 16, "POA current offset B", KueblerAccess::kReadOnly, true, KueblerFormat::kSigned,
     "Bit 15 - 0: signed 16 bit integer"},
    {0x1E, 16, "POA current amplitude A", KueblerAccess::kReadOnly, true, KueblerFormat::kUnsigned,
     "Bit 15 - 0: unsigned 16 bit integer"},
    {0x20, 16, "POA current amplitude B", KueblerAccess::kReadOnly, true, KueblerFormat::kUnsigned,
     "Bit 15 - 0: unsigned 16 bit integer"},
    {0x22, 16, "POA current phase", KueblerAccess::kReadOnly, true, KueblerFormat::kSigned,
     "Bit 15 - 0: signed 16 bit integer"},
    {0x24, 8, "POA status", KueblerAccess::kReadOnly, true, KueblerFormat::kBitField,
     "Bit 7 - 3: Reserved; Bit 2: POA saving active (1 = saving active / 0 = saving not active); "
     "Bit 1: POA fast mode (1 = fast mode active / 0 = fast mode inactive); Bit 0: POA active (1 = "
     "POA active / 0 = POA inactive)"},
    {0x25, 8, "POA control", KueblerAccess::kWriteOnly, true, KueblerFormat::kBitField,
     "Bit 7 - 3: Reserved; Bit 2: POA save values as startup; Bit 1: POA enable/disable fast mode "
     "(1 = enable / 0 = disable); Bit 0: POA set active/inactive (1 = set active / 0 = set "
     "inactive)"},
    {0x30, 32, "Absolute position ST", KueblerAccess::kReadOnly, true, KueblerFormat::kUnsigned,
     "Left aligned 32 bit singleturn position"},
    {0x34, 32, "Absolute position MT", KueblerAccess::kReadOnly, true, KueblerFormat::kUnsigned,
     "Right aligned 32 bit multiturn position"},
    {0x38, 32, "Velocity", KueblerAccess::kReadOnly, true, KueblerFormat::kSigned,
     "32 bit integer value (+/- FS --> +/- 125000 Hz)"},
    {0x3C, 32, "Analog value A & B", KueblerAccess::kReadOnly, true, KueblerFormat::kSignedHalves,
     "Bit 31 - 16: signed 16 bit integer (A); Bit 15 - 0: signed 16 bit integer (B)"},
    {0x40, 32, "Magnitude", KueblerAccess::kReadOnly, true, KueblerFormat::kUnsigned,
     "sin^2+cos^2 adc values"},
    {0x50, 16, "Correction control", KueblerAccess::kReadWrite, true, KueblerFormat::kBitField,
     "Bit 15 - 8: Reserved; Bit 7: Wiegand compensation active; Bit 6: Correction table active; "
     "Bit 5: Direct output; Bit 4: Save correction table; Bit 3: Erase current correction table; "
     "Bit 2: Clear current correction table; Bit 1: start linear learn; Bit 0: start automatic "
     "calibration"},
    {0x52, 16, "Correction status", KueblerAccess::kReadOnly, true, KueblerFormat::kBitField,
     "Bit 15 - 11: Reserved; Bit 10: learning direction; Bit 9: Processing active; Bit 8: Sampling "
     "active; Bit 7: Wiegand compensation active; Bit 6: Correction table active; Bit 5: Direct "
     "output active; Bit 4: Save correction table active; Bit 3: Erase current correction table "
     "active; Bit 2: Reserved; Bit 1: linear learn active; Bit 0: automatic calibration active"},
    {0x54, 8, "Configuration control", KueblerAccess::kWriteOnly, true, KueblerFormat::kBitField,
     "Bit 7 - 2: Reserved; Bit 1: Load default configuration; Bit 0: Save current configuration"},
    {0x60, 8, "Error status", KueblerAccess::kReadOnly, false, KueblerFormat::kBitField,
     "Bit 7 - 3: Reserved; Bit 2: Command execution error; Bit 1: signal error; Bit 0: "
     "Initialization error"},
    {0x61, 8, "Error control", KueblerAccess::kWriteOnly, false, KueblerFormat::kBitField,
     "Bit 7 - 2: Reserved; Bit 1: Clear all errors; Bit 0: Clear single error"},
    {0x62, 32, "Error code 1", KueblerAccess::kReadOnly, false, KueblerFormat::kUnsigned, "TBD"},
    {0x66, 32, "Error code 2", KueblerAccess::kReadOnly, false, KueblerFormat::kUnsigned, "TBD"},
    {0x6A, 32, "Error code 3", KueblerAccess::kReadOnly, false, KueblerFormat::kUnsigned, "TBD"},
});

/// @brief The register at @p address, or @c std::nullopt when the draft documents none.
///
/// A missing entry is not an error: the draft is preliminary and the command addresses any byte, so
/// an unknown address is read as an unnamed register rather than refused.
constexpr std::optional<KueblerRegister> findKueblerRegister(uint8_t address) {
  const auto entry = std::ranges::find(kKueblerRegisters, address, &KueblerRegister::address);
  if (entry == kKueblerRegisters.end()) {
    return std::nullopt;
  }
  return *entry;
}

/// @brief The largest register width OS command 19 can transfer, in bytes (its length byte is
///        capped at 4). Register 0x04 is 64-bit and therefore out of reach in one command.
inline constexpr uint8_t kMaxKueblerRegisterBytes = 4;

}  // namespace mm::node::somanet
