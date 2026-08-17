#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string_view>

namespace mm::node::somanet {

/// @file
/// @brief The register maps of the two iC-Haus chips inside a Circulo's internal encoder —
///        reference data for OS command 0, and documentation in its own right.
///
/// Pure data with no dependency on @c Device, like @c kuebler_registers.h, which does the same job
/// for the Integro's internal (Kübler) encoder. Served as-is by @c GET
/// @c /api/meta/ic-haus-registers.
///
/// **Two chips, and the iC-MU is the one in charge.** The iC-MU is the position encoder proper: a
/// magnetic off-axis absolute chip speaking BiSS-C to the drive. The iC-PVL is a battery-buffered
/// Hall multiturn counter behind it, counting revolutions while the drive is unpowered; the drive
/// reaches it over I²C *through* the iC-MU, never directly, using the device ids the iC-MU's
/// @c I2C_DEVID register (0x5E) carries. At startup the firmware syncs the two — reading the
/// iC-PVL's @c SYNC bits, converting them to an angle, and correcting the iC-MU's @c SPO_MT.
///
/// **The space is part of a register's identity, not a heading.** Addresses collide across the
/// five spaces below — iC-PVL 0x00 is @c EN_PAR/EN_ERR/DIR/… in EEPROM and Table 6, and
/// @c PRESET/PDR/BAT_WRN/… in the limited I²C window — so an address alone names nothing.
///
/// Transcribed from the vendors' datasheets: iC-MU Series Rev B1 (Tables 90 and 91) and iC-PVL
/// Rev F2 (Tables 5, 6 and 7). Bit layouts are the datasheets' own field names, most significant
/// bit first; a row spanning several addresses is kept as one row, exactly as the datasheets print
/// it. What a field *means* is not reproduced here — that is the datasheets' CONFIGURATION
/// PARAMETERS chapters, and this table is the map, not the manual.

/// @brief Which chip a register space belongs to.
enum class IcHausChip : uint8_t {
  kIcMu,   ///< The position encoder; BiSS-C to the drive.
  kIcPvl,  ///< The battery-buffered multiturn counter, reached over I²C through the iC-MU.
};

/// @brief Name of a chip, as the console and logs should render it. Never returns @c nullptr.
constexpr std::string_view toString(IcHausChip chip) {
  switch (chip) {
    case IcHausChip::kIcMu:
      return "iC-MU";
    case IcHausChip::kIcPvl:
      return "iC-PVL";
  }
  return "unknown";
}

/// @brief One row of a register map — usually one address, sometimes a printed range.
struct IcHausRegister {
  uint8_t address = 0;  ///< The address, or the first of a range.

  /// The last address of the range, equal to @c address for the ordinary single-address row. The
  /// datasheets print a block of identical rows once (`0x30 … 0x3F RESERVED`, the sixteen
  /// @c USER_EXCHANGE_REGISTERS at 0x60-0x6F), and collapsing them the same way keeps the
  /// transcription honest instead of inventing fifteen rows the vendor never wrote.
  uint8_t lastAddress = 0;

  /// The datasheet's bit layout, most significant bit first, fields joined with " | ". Empty when
  /// @c reserved. A field carries its own bit range where the datasheet gives one, so
  /// `GC_M(1:0) | GF_M(5:0)` is read exactly as printed.
  std::string_view fields;

  bool reserved = false;  ///< The whole row is reserved; @c fields is empty.

  /// Reachable over SPI only, so OS command 0 cannot touch it. True for the iC-MU's 0x80-0xAF,
  /// which the datasheet marks "access on address space SER > 0x7F only via SPI interface
  /// possible" — a client should show these rather than offer them.
  bool spiOnly = false;
};

/// @brief Serialises an @c IcHausRegister. Participates in nlohmann ADL, so a span of them
///        converts on its own.
void to_json(nlohmann::json& j, const IcHausRegister& r);

/// @brief One addressable register space of one chip.
struct IcHausRegisterSpace {
  IcHausChip chip{IcHausChip::kIcMu};
  std::string_view name;  ///< The datasheet's own name for the space.

  /// How the space is reached, since that is what decides whether OS command 0 can read it at all.
  std::string_view addressing;

  std::span<const IcHausRegister> registers;
};

/// @brief Serialises an @c IcHausRegisterSpace, registers included.
void to_json(nlohmann::json& j, const IcHausRegisterSpace& space);

/// @brief Every register space of both chips, in the order a reader should meet them: the iC-MU's
///        two, then the iC-PVL's three.
std::span<const IcHausRegisterSpace> icHausRegisterSpaces();

}  // namespace mm::node::somanet
