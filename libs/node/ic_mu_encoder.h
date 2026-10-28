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

#include "node/cia402_drive.h"
#include "node/ic_haus_registers.h"
#include "node/somanet_drive.h"

namespace mm::node::somanet {

/// @file
/// @brief What configuring a Circulo's integrated iC-MU encoder writes, as pure functions and data.
///
/// The drive firmware does not configure the iC-MU or the iC-PVL beside it. The master does, one
/// register at a time with OS command 0, and the procedure body sequences the writes from here. No
/// value comes from a product table: the singleturn bits @c st and the multiturn bits @c mt of the
/// encoder configuration decide every value that differs between encoders. The register values
/// that do not depend on the encoder, and the order of the stages, are fixed, and a trace of a
/// configuration on a Circulo 7 or a Circulo 9 is the reference for them.
///
/// The iC-PVL is reached through the iC-MU: a value goes into the exchange registers 0x60-0x6F,
/// and @c I2C_COM moves it over I²C to the device that @c I2C_DEVID (0x5E) names. iC-MU datasheet
/// Rev B1, Tables 103-106.

/// @brief iC-MU register 0x0F: @c SPO_MT in bits 7:4, @c MPC in bits 3:0.
inline constexpr uint8_t kIcMuMpcRegister = 0x0F;

/// @brief iC-MU register 0x11: @c OUT_ZERO in bits 7:5, @c OUT_MSB in bits 4:0.
inline constexpr uint8_t kIcMuOutputRegister = 0x11;

/// @brief What register 0x11 holds in an iC-MU with no configuration in its EEPROM (iC-MU
/// datasheet,
///        Table 27).
inline constexpr uint8_t kIcMuUnconfiguredOutput = 0xA5;

/// @brief iC-MU register 0x74, @c HARD_REV: the part and its silicon revision.
inline constexpr uint8_t kIcMuHardRevRegister = 0x74;

/// @brief iC-MU register 0x75, @c CMD_MU. A write runs a command rather than storing a value.
inline constexpr uint8_t kIcMuCommandRegister = 0x75;

/// @brief The first exchange register. A one-byte I²C transfer reads into it or writes from it.
inline constexpr uint8_t kIcMuExchangeRegister = 0x60;

/// @brief The commands the configuration writes to @c CMD_MU (iC-MU datasheet, Table 102).
enum class IcMuCommand : uint8_t {
  kWriteAll = 0x01,  ///< @c WRITE_ALL: copy the RAM configuration into the EEPROM.
  kAbsReset = 0x03,  ///< @c ABS_RESET: compute the absolute position again.
  kI2cCom = 0x0A,    ///< @c I2C_COM: run the I²C transfer set up in 0x5B-0x5E.
};

/// @brief The commands the configuration writes to the iC-PVL @c CMD register, 0x11 (iC-PVL
///        datasheet Rev F2, Table 33).
enum class IcPvlCommand : uint8_t {
  kReboot = 0x03,  ///< Load the configuration again from the EEPROM.
  kSclr = 0x05,    ///< Clear the status bits.
};

/// @brief One write of one iC-MU register, with OS command 0.
struct IcMuRegisterWrite {
  uint8_t address = 0;
  uint8_t value = 0;

  bool operator==(const IcMuRegisterWrite&) const = default;
};

/// @brief Whether the configuration sets up the iC-PVL for multiturn.
enum class IcMuPath : uint8_t {
  kSingleturn,
  kMultiturn,
};

/// @brief The path for an encoder on @p sensorPort with @p multiturnBits.
///
/// Only sensor port 1 carries an iC-PVL, so an encoder on port 2 takes the singleturn path
/// whatever its multiturn bits say.
IcMuPath icMuPath(uint8_t sensorPort, uint8_t multiturnBits);

/// @brief The lowest and the highest singleturn bits an iC-MU ring delivers.
///
/// 16, 32 and 64 master pole pairs are 18, 19 and 20 bits (iC-MU datasheet, Table 51). Every rule
/// below holds for these three and is refused outside them, because a fallback to another ring's
/// values configures the chip for a ring it does not have.
inline constexpr uint8_t kIcMuMinSingleturnBits = 18;
inline constexpr uint8_t kIcMuMaxSingleturnBits = 20;

/// @brief Whether @p singleturnBits is a ring the rules know.
constexpr bool isIcMuSingleturnBits(uint8_t singleturnBits) {
  return singleturnBits >= kIcMuMinSingleturnBits && singleturnBits <= kIcMuMaxSingleturnBits;
}

/// @brief Why @p singleturnBits cannot be configured, or nothing when it can.
std::optional<std::string> icMuSingleturnBitsError(uint8_t singleturnBits);

/// @brief The master pole pairs of the ring, 2^(st − 14). Requires @c isIcMuSingleturnBits.
uint8_t icMuMasterPolePairs(uint8_t singleturnBits);

/// @brief @c MPC, the low nibble of register 0x0F: st − 14. Requires @c isIcMuSingleturnBits.
uint8_t icMuMasterPeriodCount(uint8_t singleturnBits);

/// @brief @c OUT_MSB with @c OUT_ZERO 0, the whole of register 0x11. Requires
///        @c isIcMuSingleturnBits.
///
/// The output word ends at bit 13 plus this value, so it is st − 14 on the singleturn path and
/// st + mt − 14 on the multiturn path. @p multiturnBits is ignored on the singleturn path.
uint8_t icMuOutMsb(IcMuPath path, uint8_t singleturnBits, uint8_t multiturnBits);

/// @brief @c PCR, iC-PVL register 0x02: the master periods per revolution less one,
///        2^(st − 14) − 1. Requires @c isIcMuSingleturnBits.
uint8_t icPvlPeriodCountRatio(uint8_t singleturnBits);

/// @brief Register 0x0B, which sets the two output ports of the iC-MU.
///
/// Port A is BiSS in both values. On a Safe Motion drive port B carries the analog sine and cosine
/// of the master track, @c MODEB 0x5, and otherwise ABZ, @c MODEB 0x0 (iC-MU datasheet, Tables 28
/// and 29). @p safeMotion is whether the object dictionary contains 0x2630, which only a Safe
/// Motion build has.
uint8_t icMuOutputPorts(bool safeMotion);

/// @brief Which shaft an encoder measures.
enum class EncoderShaft : uint8_t {
  kMotor,
  kDriving,
};

/// @brief Name of a shaft, as a message renders it. Never returns @c nullptr.
constexpr std::string_view toString(EncoderShaft shaft) {
  switch (shaft) {
    case EncoderShaft::kMotor:
      return "motor";
    case EncoderShaft::kDriving:
      return "driving";
  }
  return "unknown";
}

/// @brief The shaft @p encoder measures, from the controller it serves, or nothing when it serves
///        none.
///
/// The commutation encoder is on the motor. A position or velocity encoder is on the driving shaft
/// when a gearbox sits between the two, and on the motor otherwise. A gearbox counts only when the
/// position encoder, or without one the velocity encoder, is not the commutation encoder, because
/// a gear ratio between two readings of one encoder means nothing. Then it counts when
/// @p gearRatio is above 1. A ratio with a zero term is read as 1.
std::optional<EncoderShaft> encoderShaft(const Encoders& encoders, EncoderOrdinal encoder,
                                         const GearRatio& gearRatio);

/// @brief One battery mode of the iC-PVL: the highest acceleration it detects, and the current it
///        draws from the battery for that.
///
/// @c code is the value of iC-PVL register 0x03. The accelerations are the ones Synapticon gives
/// for a Circulo, one column for each ring. No value is known for 16 master pole pairs, so that
/// ring offers no mode.
struct IcPvlBatteryMode {
  std::string_view name;            ///< The token a request names it by.
  uint8_t code = 0;                 ///< iC-PVL register 0x03.
  uint8_t maxCurrentMicroamps = 0;  ///< The current the iC-PVL draws from the battery, in µA.

  /// The highest acceleration the iC-PVL follows, in krpm/s, for 32 and for 64 master pole pairs.
  std::array<double, 2> maxAccelerationKrpmPerSecond{};
};

/// @brief Every battery mode, from the lowest current to the highest.
std::span<const IcPvlBatteryMode> icPvlBatteryModes();

/// @brief The highest acceleration of @p mode on the ring of @p singleturnBits, in krpm/s, or
///        nothing for a ring that offers no mode.
std::optional<double> icPvlMaxAcceleration(const IcPvlBatteryMode& mode, uint8_t singleturnBits);

/// @brief The battery mode of an encoder on @p shaft when the request names none.
uint8_t icPvlDefaultBatteryMode(EncoderShaft shaft);

/// @brief The CRC the iC-PVL checks over its configuration: polynomial 0x2F, init 0x00, xorout
///        0xFF, not reflected.
uint8_t icPvlCrc8(std::span<const uint8_t> data);

/// @brief What one configuration writes, once the request and the drive have been read.
struct IcMuConfiguration {
  IcMuPath path{IcMuPath::kSingleturn};
  uint8_t singleturnBits = 0;  ///< Must satisfy @c isIcMuSingleturnBits.
  uint8_t multiturnBits = 0;   ///< Ignored on the singleturn path.
  bool safeMotion = false;     ///< See @c icMuOutputPorts.
  uint8_t batteryMode = 0;     ///< iC-PVL register 0x03. Ignored on the singleturn path.
};

/// @brief The iC-MU configuration registers 0x00-0x18 that the configuration owns, in RAM.
///
/// The calibration owns 0x01-0x04 and 0x07-0x0A, so they are not written.
std::vector<IcMuRegisterWrite> icMuRamWrites(const IcMuConfiguration& configuration);

/// @brief The iC-PVL configuration into EEPROM 0x40-0x4C, ending with the @c I2C_COM that writes
///        it.
///
/// The thirteen bytes are iC-PVL registers 0x00-0x0C. Registers 0x06 and 0x0C are the CRCs over
/// 0x00-0x05 and 0x07-0x0B, computed over the bytes actually written.
std::vector<IcMuRegisterWrite> icPvlEepromWrites(const IcMuConfiguration& configuration);

/// @brief Writes @p value into iC-PVL register @p icPvlRegister, ending with the @c I2C_COM.
std::vector<IcMuRegisterWrite> icPvlRegisterWrite(uint8_t icPvlRegister, uint8_t value);

/// @brief Runs @p command on the iC-PVL, ending with the @c I2C_COM.
std::vector<IcMuRegisterWrite> icPvlCommand(IcPvlCommand command);

/// @brief Copies iC-PVL register @p icPvlRegister into @c kIcMuExchangeRegister, ending with the
///        @c I2C_COM. A read of the exchange register returns the value.
std::vector<IcMuRegisterWrite> icPvlRegisterRead(uint8_t icPvlRegister);

/// @brief iC-PVL register 0x05 with @c I2C_POS set, so that register 0x0D reports the @c SYNC bits.
inline constexpr uint8_t kIcPvlPositionOutputOn = 0xB0;

/// @brief iC-PVL register 0x05 as the configuration leaves it.
inline constexpr uint8_t kIcPvlPositionOutputOff = 0x30;

/// @brief The iC-PVL register that carries the @c SYNC bits in bits 2:0.
inline constexpr uint8_t kIcPvlSyncRegister = 0x0D;

/// @brief Register 0x0F with @c SPO_MT set so that the iC-MU and the iC-PVL agree on the
///        revolution. Requires @c isIcMuSingleturnBits.
///
/// The @c SYNC bits count steps of 45°, the singleturn position gives the iC-MU angle, and @c
/// SPO_MT is the difference between the two in steps of 22.5°, rounded to the nearest step. A
/// difference of sixteen steps wraps to 0.
///
/// @param rawPosition     The encoder's raw position, 0x2111:01 or 0x2113:01. Only the singleturn
///                        bits are used.
/// @param singleturnBits  The singleturn bits of the encoder.
/// @param icPvlSync       iC-PVL register 0x0D. Only bits 2:0 are used.
uint8_t icMuSpoMtRegister(uint32_t rawPosition, uint8_t singleturnBits, uint8_t icPvlSync);

/// @brief Whether the status lists @p encoder: a BiSS or internal encoder on sensor port 1 or 2,
///        with singleturn bits.
///
/// Only those two ports carry an iC-MU. An encoder with 0 singleturn bits has no :0C, which is a
/// board that carries no iC-MU, such as an NGC board with Jasper firmware. The type does not say
/// which chip is behind the port, so a listed encoder is one the configuration can address, not one
/// known to be an iC-MU. @c HARD_REV in the status says that.
bool isIcMuEncoder(const EncoderConfiguration& encoder);

/// @brief Whether an iC-MU holds the output configuration the rules expect.
enum class IcMuEncoderState : uint8_t {
  kNotConfigured,  ///< Register 0x11 holds @c kIcMuUnconfiguredOutput.
  kInvalid,        ///< Register 0x11 holds another value than the expected one.
  kConfigured,     ///< Register 0x11 holds the expected value.
};

/// @brief Name of a state, as the API renders it. Never returns @c nullptr.
constexpr std::string_view toString(IcMuEncoderState state) {
  switch (state) {
    case IcMuEncoderState::kNotConfigured:
      return "not-configured";
    case IcMuEncoderState::kInvalid:
      return "invalid";
    case IcMuEncoderState::kConfigured:
      return "configured";
  }
  return "unknown";
}

/// @brief The state of an iC-MU whose register 0x11 holds @p outputRegister.
///
/// Without an expected value, only @c kNotConfigured can be decided. Every other value is nothing,
/// because a value cannot be called invalid against a rule that does not apply to the ring.
std::optional<IcMuEncoderState> icMuEncoderState(uint8_t outputRegister,
                                                 std::optional<uint8_t> expected);

/// @brief The part that @p hardRev names in its upper nibble, or nothing for a code the iC-MU
///        datasheet does not list (Rev B1, Table 92).
std::optional<std::string_view> icMuPartName(uint8_t hardRev);

/// @brief One battery mode a configuration request can name for an encoder, with the acceleration
///        it allows on that encoder's ring.
struct IcPvlBatteryModeChoice {
  std::string_view name;
  uint8_t maxCurrentMicroamps = 0;
  double maxAccelerationKrpmPerSecond = 0;
};
void to_json(nlohmann::json& j, const IcPvlBatteryModeChoice& choice);

/// @brief What the status reports for one encoder.
struct IcMuEncoderStatus {
  EncoderOrdinal encoderOrdinal{EncoderOrdinal::kEncoder1};
  uint8_t sensorPort = 0;                         ///< 0x21x0:01.
  uint8_t singleturnBits = 0;                     ///< 0x21x0:0C.
  uint8_t multiturnBits = 0;                      ///< 0x21x0:0A, 0 where the object has none.
  std::optional<uint8_t> masterPolePairs;         ///< Empty for a ring the rules do not know.
  uint8_t hardRev = 0;                            ///< iC-MU register 0x74.
  uint8_t outputRegister = 0;                     ///< iC-MU register 0x11.
  std::optional<uint8_t> expectedOutputRegister;  ///< Empty for a ring the rules do not know.
  std::optional<IcMuEncoderState> state;          ///< See @c icMuEncoderState.
  std::optional<std::string> reason;              ///< Why there is no expected value.
  IcMuPath path{IcMuPath::kSingleturn};           ///< The path a configuration takes.

  /// The battery modes a configuration request can name besides the default. Empty on the
  /// singleturn path, and on a ring that offers no mode.
  std::vector<IcPvlBatteryModeChoice> batteryModes;
};
void to_json(nlohmann::json& j, const IcMuEncoderStatus& status);

/// @brief The status of @p encoder, from the two iC-MU registers the status reads.
IcMuEncoderStatus icMuEncoderStatus(const EncoderConfiguration& encoder, uint8_t hardRev,
                                    uint8_t outputRegister);

/// @brief The RAM addresses of the registers in the restore file, in the order the file holds them.
///
/// They are the iC-MU configuration 0x00-0x18 and 0x52-0x59. The drive writes the file to the
/// encoder when the iC-MU reports a corrupted EEPROM, so the file holds the calibration registers
/// as well as the ones the configuration writes.
std::span<const uint8_t> icMuRestoreRegisters();

/// @brief The name of the restore file for the encoder on @p sensorPort.
///
/// The drive picks the file by the sensor port, not by the encoder ordinal.
std::string icMuRestoreFileName(uint8_t sensorPort);

/// @brief The restore file: one @c [address, value] pair for each register of
///        @c icMuRestoreRegisters. @p values holds the value of each register in that order.
std::vector<uint8_t> icMuRestoreFile(std::span<const uint8_t> values);

/// @brief One @c [address, value] pair of a restore file, with the iC-MU register at that address.
struct IcMuRestoreEntry {
  uint8_t address = 0;
  uint8_t value = 0;
  const IcHausRegister* icMuRegister = nullptr;  ///< Nothing for an address the map does not list.
};
void to_json(nlohmann::json& j, const IcMuRestoreEntry& entry);

/// @brief The largest restore file the drive takes: 33 pairs.
inline constexpr size_t kIcMuRestoreFileMaxBytes = 66;

/// @brief Decodes a restore file into its pairs.
///
/// The drive refuses a file of odd length or over @c kIcMuRestoreFileMaxBytes with error 0x6320,
/// so this refuses the same files.
std::expected<std::vector<IcMuRestoreEntry>, std::string> parseIcMuRestoreFile(
    std::span<const uint8_t> bytes);

/// @brief Whether a firmware can run the configuration.
enum class IcMuFirmwareSupport : uint8_t {
  kSupported,
  kTooOld,      ///< Older than 5.0.0-rc.5, and not calibration firmware.
  kJasper,      ///< 10.0.0 or newer. Jasper has no iC-PVL, so only the singleturn path runs.
  kUnreadable,  ///< Not a version string.
};

/// @brief Whether the firmware that reports @p version in 0x100A can run the configuration.
///
/// The configuration needs 5.0.0-rc.5 or newer. Calibration firmware, 0.4.0 up to 1.0.0-alpha.0,
/// can also run it. A leading 'v' is accepted.
IcMuFirmwareSupport icMuFirmwareSupport(std::string_view version);

}  // namespace mm::node::somanet
