#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
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
/// Rev F2 (Tables 5, 6 and 7). A register carries the datasheet's own fields, most significant bit
/// first; a row spanning several addresses is kept as one row, exactly as the datasheets print it.
///
/// **Each field's description is the datasheet's own one-liner** for it, from the CONFIGURATION
/// PARAMETERS index (iC-MU p. 18-19, iC-PVL p. 12) and, for the status registers the iC-PVL breaks
/// out bit by bit, from its status byte tables. The prose *behind* those lines is not reproduced —
/// this is the map with its legend, not the manual.
///
/// **Granularity follows each datasheet's own register map.** The iC-PVL prints its status
/// registers bit by bit, so those appear here as eight fields; the iC-MU prints its own as
/// @c STATUS0(7:0), so that is one field. Neither is re-cut to match the other, because the map is
/// what a reader will have open beside this.

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

/// @brief One field of a register — a named piece of it, with what the datasheet calls it.
struct IcHausField {
  std::string_view name;  ///< The datasheet's own name, e.g. "GC_M".

  /// The field's bit slice **as the register map prints it**, e.g. "1:0" — empty where the map
  /// prints a bare name. This is the slice *of the field* the byte carries, not the field's
  /// position within the byte: `GC_M(1:0)` occupies bits 7:6 of address 0x00 and says "1:0"
  /// because those are the field's own two bits. A multi-byte field is how a value wider than a
  /// register is spread across several, which is why `RESABZ` reads "7:0" at 0x13 and "15:8" at
  /// 0x14.
  std::string_view bits;

  std::string_view description;  ///< The datasheet's own one-line description of the field.
};

/// @brief Serialises an @c IcHausField. Participates in nlohmann ADL.
void to_json(nlohmann::json& j, const IcHausField& field);

/// @brief How many fields one register row can name.
///
/// Eight: a status register the datasheet breaks out bit by bit is the widest case, and no register
/// can name more than its own bits. A fixed array rather than a span so a register row and its
/// fields are one literal — the alternative is a named array per register, which is 150 names for
/// no gain.
inline constexpr size_t kMaxFieldsPerRegister = 8;

/// @brief One row of a register map — usually one address, sometimes a printed range.
struct IcHausRegister {
  uint8_t address = 0;  ///< The address, or the first of a range.

  /// The last address of the range, equal to @c address for the ordinary single-address row. The
  /// datasheets print a block of identical rows once (`0x30 … 0x3F RESERVED`, the sixteen
  /// @c USER_EXCHANGE_REGISTERS at 0x60-0x6F), and collapsing them the same way keeps the
  /// transcription honest instead of inventing fifteen rows the vendor never wrote.
  uint8_t lastAddress = 0;

  bool reserved = false;  ///< The whole row is reserved and names no fields.

  /// Reachable over SPI only, so OS command 0 cannot touch it. True for the iC-MU's 0x80-0xAF,
  /// which the datasheet marks "access on address space SER > 0x7F only via SPI interface
  /// possible" — a client should show these rather than offer them.
  bool spiOnly = false;

  std::array<IcHausField, kMaxFieldsPerRegister> fields{};  ///< Only the first @c fieldCount count.
  uint8_t fieldCount = 0;  ///< How many of @c fields this row actually names.

  /// @brief The fields this row names, most significant bit first.
  std::span<const IcHausField> namedFields() const { return {fields.data(), fieldCount}; }

  /// @brief The row as the datasheet prints it — field names with their bit slices, joined with
  ///        " | ". Derived rather than stored, so it can never disagree with @c fields.
  std::string layout() const;
};

/// @brief Serialises an @c IcHausRegister, @c layout() included. Participates in nlohmann ADL, so a
///        span of them converts on its own.
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
