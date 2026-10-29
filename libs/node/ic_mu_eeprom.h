#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string_view>
#include <vector>

#include "node/ic_mu_encoder.h"

namespace mm::node::somanet {

/// @file
/// @brief The EEPROM of an iC-MU encoder and the I²C bridge that reaches it.
///
/// The iC-MU and the iC-PVL load their configuration from one external I²C EEPROM. The drive
/// cannot reach that EEPROM, or the iC-PVL, directly: the iC-MU's own I²C master does it. A
/// transfer is set up in 0x5B-0x5E, its data passes through the exchange registers 0x60-0x6F, and
/// I2C_COM starts it (iC-MU datasheet Rev B1, Tables 103-106).

/// @brief What an I²C transfer of the iC-MU addresses.
enum class IcMuI2cTarget : uint8_t {
  kEeprom,       ///< The EEPROM, device id 0b1010 000 (iC-MU datasheet, Table 103).
  kIcPvl,        ///< Every iC-PVL register, device id 0b1100 001 (iC-PVL datasheet, Table 6).
  kIcPvlStatus,  ///< The iC-PVL status and command only, id 0b1100 000 (iC-PVL datasheet, Table 7).
};

/// @brief The request name of @p target: "eeprom", "ic-pvl" or "ic-pvl-status".
std::string_view toString(IcMuI2cTarget target);

/// @brief The target a request names, or nothing for a name that is not one.
std::optional<IcMuI2cTarget> parseIcMuI2cTarget(std::string_view name);

/// @brief The value of I2C_DEVID (0x5E) for @p target: the device id with the R/W bit.
uint8_t icMuI2cDeviceId(IcMuI2cTarget target, bool write);

/// @brief I2C_DEV_START, I2C_RAM_START, I2C_RAM_END and I2C_DEVID.
inline constexpr uint8_t kIcMuI2cDeviceStartRegister = 0x5B;
inline constexpr uint8_t kIcMuI2cRamStartRegister = 0x5C;
inline constexpr uint8_t kIcMuI2cRamEndRegister = 0x5D;
inline constexpr uint8_t kIcMuI2cDeviceIdRegister = 0x5E;

/// @brief The bytes one transfer moves at most: the exchange registers 0x60-0x6F.
inline constexpr uint8_t kIcMuExchangeBytes = 16;

/// @brief The writes that set up a transfer of @p count bytes at @p deviceStart, through the
///        exchange registers from 0x60. Requires 1 ≤ @p count ≤ @c kIcMuExchangeBytes. I2C_COM is
///        not among them.
std::vector<IcMuRegisterWrite> icMuI2cTransferSetup(uint8_t deviceId, uint8_t deviceStart,
                                                    uint8_t count);

/// @brief The last EEPROM address that holds configuration. 0x00-0x3F is the iC-MU's bank 0, and
///        0x40-0x4C the 13 bytes of the iC-PVL, so 0x00-0x4C covers both chips.
inline constexpr uint8_t kIcMuEepromConfigurationEnd = 0x4C;

/// @brief The write page an EEPROM write stays inside.
///
/// The iC-PVL takes an EEPROM from 24C01 to 24C16 (iC-PVL datasheet Rev F2, p. 26). The smallest
/// write page among them is 8 bytes, and a write past the end of its page wraps to the start of the
/// page. Which part a board carries is not known here, so every write stays inside 8 aligned bytes.
inline constexpr uint8_t kIcMuEepromWritePage = 8;

/// @brief The EEPROM address of the iC-MU RAM register @p ramAddress, or nothing for a register
///        the EEPROM does not hold.
///
/// RAM 0x00-0x18 is EEPROM 0x00-0x18, and RAM 0x52-0x59, SPO_BASE and SPO_0-14, is EEPROM
/// 0x19-0x20 (iC-MU datasheet Rev B1, Figure 51).
std::optional<uint8_t> icMuEepromAddress(uint8_t ramAddress);

/// @brief One key register of an iC-MU: a register of the restore file, read from RAM.
struct IcMuKeyRegister {
  uint8_t ramAddress = 0;
  uint8_t eepromAddress = 0;  ///< Where the EEPROM keeps it, from @c icMuEepromAddress.
  uint8_t value = 0;
};
void to_json(nlohmann::json& j, const IcMuKeyRegister& entry);

/// @brief One byte of the EEPROM.
struct IcMuEepromByte {
  uint8_t address = 0;
  uint8_t value = 0;
};
void to_json(nlohmann::json& j, const IcMuEepromByte& entry);

/// @brief One I²C transfer of a range: @p count bytes from @p deviceStart.
struct IcMuI2cChunk {
  uint8_t deviceStart = 0;
  uint8_t count = 0;

  bool operator==(const IcMuI2cChunk&) const = default;
};

/// @brief The transfers that read @p first to @p last, inclusive, at most @c kIcMuExchangeBytes
///        each. Requires @p first ≤ @p last.
std::vector<IcMuI2cChunk> icMuEepromReadChunks(uint8_t first, uint8_t last);

/// @brief The transfers that write @p first to @p last, inclusive, each inside one aligned
///        @c kIcMuEepromWritePage. Requires @p first ≤ @p last.
std::vector<IcMuI2cChunk> icMuEepromWriteChunks(uint8_t first, uint8_t last);

}  // namespace mm::node::somanet
