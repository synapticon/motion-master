#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "node/ic_mu_eeprom.h"
#include "node/ic_mu_encoder.h"
#include "node/somanet_drive.h"

namespace mm::node {

/// @file
/// @brief The registers of an iC-MU encoder, and the I²C bridge behind them, over OS command 0.
///
/// Free functions over a @c SomanetDrive, not methods of it, because the iC-MU types build on
/// @c somanet_drive.h. Each function sends one OS command 0 at a time and holds no lock between
/// them. A procedure that uses the bridge must first turn the BiSS status bits off with OS
/// command 14, because the drive refuses 0x5B-0x6F otherwise.

/// @brief Bits of iC-MU STATUS1, register 0x77 (iC-MU datasheet Rev B1, Table 97).
inline constexpr uint8_t kIcMuStatus1Register = 0x77;
inline constexpr uint8_t kIcMuCrcError = 0x80;
inline constexpr uint8_t kIcMuEepromError = 0x40;
inline constexpr uint8_t kIcMuCommandInProgress = 0x01;

/// @brief Reads one iC-MU register. A read always reports a value, so a missing one is an error.
std::expected<uint8_t, std::string> readIcMuRegister(SomanetDrive& drive,
                                                     somanet::EncoderOrdinal encoder,
                                                     uint8_t registerAddress,
                                                     const std::stop_token& stop);

/// @brief Writes one iC-MU register and compares the echo. A command to 0x75 runs rather than
///        stores, so its echo says nothing about the write.
std::expected<void, std::string> writeIcMuRegister(SomanetDrive& drive,
                                                   somanet::EncoderOrdinal encoder,
                                                   somanet::IcMuRegisterWrite write,
                                                   const std::stop_token& stop);

/// @brief Writes @p writes in order, and stops at the first that fails.
std::expected<void, std::string> writeIcMuRegisters(
    SomanetDrive& drive, somanet::EncoderOrdinal encoder,
    std::span<const somanet::IcMuRegisterWrite> writes, const std::stop_token& stop);

/// @brief The write of @p command to CMD_MU, 0x75.
somanet::IcMuRegisterWrite icMuCommand(somanet::IcMuCommand command);

/// @brief Refuses an iC-MU whose BANKSEL (0x40) is not 0.
///
/// Addresses 0x00-0x3F reach the bank BANKSEL selects, so with another bank a write meant for the
/// configuration lands in the EEPROM banks, and the read-back of the same address still matches
/// (iC-MU datasheet Rev B1, Table 88).
std::expected<void, std::string> checkIcMuBank(SomanetDrive& drive, somanet::EncoderOrdinal encoder,
                                               const std::stop_token& stop);

/// @brief Runs one iC-MU command, waits until it is done, and returns STATUS1.
///
/// CMD_MU keeps the command code while the command is queued and turns 0 when it starts, and
/// STATUS1 bit 0 is set while it runs (iC-MU datasheet Rev B1, p. 61 and Table 97). So the wait is
/// for both, in that order; bit 0 alone reads 0 for a command still queued. STATUS1 is read first,
/// because with the accumulated status configured it keeps a bit until it is read. EPR_ERR fails
/// the command: it reports an I²C transfer that failed, and the datasheet checks an EEPROM write
/// with it.
std::expected<uint8_t, std::string> runIcMuCommand(SomanetDrive& drive,
                                                   somanet::EncoderOrdinal encoder,
                                                   somanet::IcMuCommand command,
                                                   const std::stop_token& stop);

/// @brief Copies the RAM configuration into the EEPROM with WRITE_ALL, and returns STATUS1.
///
/// A CRC_ERR after it means the RAM the copy came from does not match its checksum.
std::expected<uint8_t, std::string> saveIcMuEeprom(SomanetDrive& drive,
                                                   somanet::EncoderOrdinal encoder,
                                                   const std::stop_token& stop);

/// @brief Runs an I²C transfer: @p writes set it up and end with I2C_COM, which runs it.
std::expected<void, std::string> runIcMuI2cTransfer(
    SomanetDrive& drive, somanet::EncoderOrdinal encoder,
    std::span<const somanet::IcMuRegisterWrite> writes, const std::stop_token& stop);

/// @brief Reads @p count bytes from @p deviceStart of @p target, in one I²C transfer.
///
/// Requires 1 ≤ @p count ≤ @c somanet::kIcMuExchangeBytes.
std::expected<std::vector<uint8_t>, std::string> readIcMuI2c(SomanetDrive& drive,
                                                             somanet::EncoderOrdinal encoder,
                                                             somanet::IcMuI2cTarget target,
                                                             uint8_t deviceStart, uint8_t count,
                                                             const std::stop_token& stop);

/// @brief Writes @p bytes to @p target from @p deviceStart, in one I²C transfer.
///
/// Requires 1 ≤ size ≤ @c somanet::kIcMuExchangeBytes. An EEPROM write must also stay inside one
/// page; @c somanet::icMuEepromWriteChunks splits a range so.
std::expected<void, std::string> writeIcMuI2c(SomanetDrive& drive, somanet::EncoderOrdinal encoder,
                                              somanet::IcMuI2cTarget target, uint8_t deviceStart,
                                              std::span<const uint8_t> bytes,
                                              const std::stop_token& stop);

}  // namespace mm::node
