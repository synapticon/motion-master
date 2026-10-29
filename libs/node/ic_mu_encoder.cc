#include "node/ic_mu_encoder.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <semver.hpp>
#include <string>
#include <utility>
#include <vector>

#include "node/ic_mu_eeprom.h"

namespace mm::node::somanet {

namespace {

// The iC-PVL configuration lives at EEPROM 0x40-0x4C, after the iC-MU's own bank at 0x00-0x3F.
constexpr uint8_t kIcPvlEepromStart = 0x40;
constexpr uint8_t kIcPvlCommandRegister = 0x11;

constexpr uint8_t kMotorShaftBatteryMode = 0x5D;
constexpr uint8_t kDrivingShaftBatteryMode = 0x68;

constexpr IcPvlBatteryMode kBatteryModes[] = {
    {.name = "6-microamperes",
     .code = 0x60,
     .maxCurrentMicroamps = 6,
     .maxAccelerationKrpmPerSecond = {3.5, 1.7}},
    {.name = "10-microamperes",
     .code = 0x58,
     .maxCurrentMicroamps = 10,
     .maxAccelerationKrpmPerSecond = {14, 7}},
    {.name = "18-microamperes",
     .code = 0x50,
     .maxCurrentMicroamps = 18,
     .maxAccelerationKrpmPerSecond = {56, 28}},
    {.name = "36-microamperes",
     .code = 0x48,
     .maxCurrentMicroamps = 36,
     .maxAccelerationKrpmPerSecond = {226, 113}},
    {.name = "72-microamperes",
     .code = 0x40,
     .maxCurrentMicroamps = 72,
     .maxAccelerationKrpmPerSecond = {895, 447}},
};

constexpr IcMuRegisterWrite command(IcMuCommand c) {
  return {kIcMuCommandRegister, static_cast<uint8_t>(c)};
}

}  // namespace

IcMuPath icMuPath(uint8_t sensorPort, uint8_t multiturnBits) {
  return sensorPort == 1 && multiturnBits != 0 ? IcMuPath::kMultiturn : IcMuPath::kSingleturn;
}

std::optional<std::string> icMuSingleturnBitsError(uint8_t singleturnBits) {
  if (isIcMuSingleturnBits(singleturnBits)) {
    return std::nullopt;
  }
  return std::format(
      "the encoder has {} singleturn bits, and an iC-MU ring has 18, 19 or 20 (16, 32 or 64 master "
      "pole pairs)",
      singleturnBits);
}

std::optional<std::string> icMuMultiturnBitsWarning(IcMuPath path, uint8_t multiturnBits) {
  if (path != IcMuPath::kMultiturn || multiturnBits == kIcMuMultiturnBits) {
    return std::nullopt;
  }
  return std::format(
      "the encoder has {} multiturn bits, and the iC-MU and the iC-PVL are configured for {}; the "
      "position word the drive reads will not match the one the encoder sends",
      multiturnBits, kIcMuMultiturnBits);
}

uint8_t icMuMasterPolePairs(uint8_t singleturnBits) {
  return static_cast<uint8_t>(1U << icMuMasterPeriodCount(singleturnBits));
}

uint8_t icMuMasterPeriodCount(uint8_t singleturnBits) {
  return static_cast<uint8_t>(singleturnBits - 14);
}

uint8_t icMuOutMsb(IcMuPath path, uint8_t singleturnBits, uint8_t multiturnBits) {
  const uint8_t outputBits =
      path == IcMuPath::kMultiturn ? singleturnBits + multiturnBits : singleturnBits;
  return static_cast<uint8_t>(outputBits - 14);
}

uint8_t icPvlPeriodCountRatio(uint8_t singleturnBits) {
  return static_cast<uint8_t>(icMuMasterPolePairs(singleturnBits) - 1);
}

uint8_t icMuOutputPorts(bool safeMotion) { return safeMotion ? 0x52 : 0x02; }

std::optional<EncoderShaft> encoderShaft(const Encoders& encoders, EncoderOrdinal encoder,
                                         const GearRatio& gearRatio) {
  if (encoders.commutation == encoder) {
    return EncoderShaft::kMotor;
  }
  if (encoders.position != encoder && encoders.velocity != encoder) {
    return std::nullopt;
  }
  const bool gearboxPossible = encoders.position ? encoders.position != encoders.commutation
                                                 : encoders.velocity != encoders.commutation;
  const bool reduces = gearRatio.motorRevolutions != 0 && gearRatio.shaftRevolutions != 0 &&
                       gearRatio.motorRevolutions > gearRatio.shaftRevolutions;
  return gearboxPossible && reduces ? EncoderShaft::kDriving : EncoderShaft::kMotor;
}

std::span<const IcPvlBatteryMode> icPvlBatteryModes() { return kBatteryModes; }

std::optional<double> icPvlMaxAcceleration(const IcPvlBatteryMode& mode, uint8_t singleturnBits) {
  switch (singleturnBits) {
    case 19:
      return mode.maxAccelerationKrpmPerSecond[0];
    case 20:
      return mode.maxAccelerationKrpmPerSecond[1];
    default:
      return std::nullopt;
  }
}

uint8_t icPvlDefaultBatteryMode(EncoderShaft shaft) {
  return shaft == EncoderShaft::kDriving ? kDrivingShaftBatteryMode : kMotorShaftBatteryMode;
}

uint8_t icPvlCrc8(std::span<const uint8_t> data) {
  uint8_t crc = 0x00;
  for (const uint8_t byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = static_cast<uint8_t>((crc & 0x80) != 0 ? (crc << 1) ^ 0x2F : crc << 1);
    }
  }
  return static_cast<uint8_t>(crc ^ 0xFF);
}

std::vector<IcMuRegisterWrite> icMuRamWrites(const IcMuConfiguration& configuration) {
  const bool multiturn = configuration.path == IcMuPath::kMultiturn;
  return {
      {0x00, 0x00},
      {0x05, 0x88},
      {0x06, 0x00},
      {0x0B, icMuOutputPorts(configuration.safeMotion)},
      // Keeps NON_CTR, the amplitude errors of both tracks, CRC_ERR and CMD_EXE out of the error
      // bit (iC-MU datasheet Rev B1, Table 100).
      {0x0C, 0x6C},
      {0x0D, 0xC0},
      {0x0E, static_cast<uint8_t>(multiturn ? 0x44 : 0x04)},
      {kIcMuMpcRegister, icMuMasterPeriodCount(configuration.singleturnBits)},
      {0x10, static_cast<uint8_t>(multiturn ? 0x6F : 0x00)},
      {kIcMuOutputRegister,
       icMuOutMsb(configuration.path, configuration.singleturnBits, configuration.multiturnBits)},
      {0x12, 0x00},
      {0x13, 0xFF},
      {0x14, 0x0F},
      {0x15, 0x13},
      {0x16, 0x10},
      {0x17, 0x02},
      {0x18, 0x00},
  };
}

std::vector<IcMuRegisterWrite> icPvlEepromWrites(const IcMuConfiguration& configuration) {
  const std::array<uint8_t, 6> settings = {
      0x28,
      0x09,
      icPvlPeriodCountRatio(configuration.singleturnBits),
      configuration.batteryMode,
      0x50,
      kIcPvlPositionOutputOff,
  };
  const std::array<uint8_t, 5> reserved{};

  // iC-PVL registers 0x00-0x0C, in the order they land in the EEPROM.
  std::vector<uint8_t> eeprom(settings.begin(), settings.end());
  eeprom.push_back(icPvlCrc8(settings));
  eeprom.insert(eeprom.end(), reserved.begin(), reserved.end());
  eeprom.push_back(icPvlCrc8(reserved));

  auto writes = icMuI2cTransferSetup(icMuI2cDeviceId(IcMuI2cTarget::kEeprom, true),
                                     kIcPvlEepromStart, static_cast<uint8_t>(eeprom.size()));
  std::ranges::transform(eeprom, std::back_inserter(writes),
                         [address = kIcMuExchangeRegister](uint8_t value) mutable {
                           return IcMuRegisterWrite{address++, value};
                         });
  writes.push_back(command(IcMuCommand::kI2cCom));
  return writes;
}

std::vector<IcMuRegisterWrite> icPvlRegisterWrite(uint8_t icPvlRegister, uint8_t value) {
  auto writes =
      icMuI2cTransferSetup(icMuI2cDeviceId(IcMuI2cTarget::kIcPvl, true), icPvlRegister, 1);
  writes.push_back({kIcMuExchangeRegister, value});
  writes.push_back(command(IcMuCommand::kI2cCom));
  return writes;
}

std::vector<IcMuRegisterWrite> icPvlCommand(IcPvlCommand c) {
  return icPvlRegisterWrite(kIcPvlCommandRegister, static_cast<uint8_t>(c));
}

std::vector<IcMuRegisterWrite> icPvlRegisterRead(uint8_t icPvlRegister) {
  auto writes =
      icMuI2cTransferSetup(icMuI2cDeviceId(IcMuI2cTarget::kIcPvl, false), icPvlRegister, 1);
  writes.push_back(command(IcMuCommand::kI2cCom));
  return writes;
}

uint8_t icMuSpoMtRegister(uint32_t rawPosition, uint8_t singleturnBits, uint8_t icPvlSync) {
  constexpr uint32_t kTenthsPerRevolution = 3600;
  constexpr uint32_t kSyncStep = 450;
  constexpr uint32_t kSpoMtStep = 225;

  const uint32_t singleturnMask = (1U << singleturnBits) - 1;
  const uint64_t singleturnPosition = rawPosition & singleturnMask;
  const auto icMuAngle =
      static_cast<int32_t>(singleturnPosition * kTenthsPerRevolution >> singleturnBits);
  const auto icPvlAngle = static_cast<int32_t>((icPvlSync & 0x07U) * kSyncStep);

  int32_t difference = icMuAngle - icPvlAngle;
  if (difference < 0) {
    difference += kTenthsPerRevolution;
  }
  uint32_t spoMt = static_cast<uint32_t>(difference) / kSpoMtStep;
  if (static_cast<uint32_t>(difference) % kSpoMtStep > kSpoMtStep / 2) {
    ++spoMt;
  }
  return static_cast<uint8_t>(((spoMt & 0x0F) << 4) | icMuMasterPeriodCount(singleturnBits));
}

bool isIcMuEncoder(const EncoderConfiguration& encoder) {
  const bool port = encoder.sensorPort >= 1 && encoder.sensorPort <= kMaxSensorPort;
  const bool type = encoder.type == EncoderType::kBiss || encoder.type == EncoderType::kInternal;
  return port && type && encoder.singleturnBits != 0;
}

std::optional<IcMuEncoderState> icMuEncoderState(uint8_t outputRegister,
                                                 std::optional<uint8_t> expected) {
  if (outputRegister == kIcMuUnconfiguredOutput) {
    return IcMuEncoderState::kNotConfigured;
  }
  if (!expected) {
    return std::nullopt;
  }
  return outputRegister == *expected ? IcMuEncoderState::kConfigured : IcMuEncoderState::kInvalid;
}

std::optional<std::string_view> icMuPartName(uint8_t hardRev) {
  // The upper nibble 0 is shared by the plain iC-MU and the iC-MU128, so those codes are named one
  // by one, as MU_3SL_defs.h lists them. From the iC-MU150 on, the nibble names the part.
  switch (hardRev) {
    case 0x05:
    case 0x06:
    case 0x07:
      return "iC-MU";
    case 0x09:
    case 0x0A:
      return "iC-MU128";
    default:
      break;
  }
  switch (hardRev >> 4) {
    case 0x1:
      return "iC-MU150";
    case 0x2:
      return "iC-MU200";
    default:
      return std::nullopt;
  }
}

void to_json(nlohmann::json& j, const IcPvlBatteryModeChoice& choice) {
  j = nlohmann::json{{"name", choice.name},
                     {"maxCurrentMicroamps", choice.maxCurrentMicroamps},
                     {"maxAccelerationKrpmPerSecond", choice.maxAccelerationKrpmPerSecond}};
}

void to_json(nlohmann::json& j, const IcMuEncoderStatus& status) {
  auto optional = [](const auto& value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
  };
  const auto part = icMuPartName(status.hardRev);
  j = nlohmann::json{
      {"encoderOrdinal", static_cast<int>(status.encoderOrdinal)},
      {"sensorPort", status.sensorPort},
      {"singleturnBits", status.singleturnBits},
      {"multiturnBits", status.multiturnBits},
      {"masterPolePairs", optional(status.masterPolePairs)},
      {"hardRev", status.hardRev},
      {"part", part ? nlohmann::json(std::string(*part)) : nlohmann::json(nullptr)},
      {"revision", status.hardRev & 0x0F},
      {"outputRegister", status.outputRegister},
      {"expectedOutputRegister", optional(status.expectedOutputRegister)},
      {"state", status.state ? nlohmann::json(std::string(toString(*status.state)))
                             : nlohmann::json(nullptr)},
      {"reason", optional(status.reason)},
      {"path", status.path == IcMuPath::kMultiturn ? "multiturn" : "singleturn"},
      {"batteryModes", status.batteryModes},
      {"encoderStatus", status.encoderStatus},
      {"encoderStatusBits", decodeIcMuEncoderStatus(status.encoderStatus)},
  };
}

namespace {

constexpr std::string_view kMultiturnRemedy =
    "Connect the multiturn battery and check its voltage. Run the iC-MU encoder configuration, "
    "then "
    "power cycle the drive. If the error stays, contact support.";

// On a fault reset the drive sends SCLR to the iC-PVL, which clears every latched iC-PVL error
// that is no longer present, and then synchronises the multiturn again (somanet_software,
// biss_service.xc, reset_fault and TRIGGER_ICPVL_SCLR). A power cycle does not reboot an iC-PVL on
// its battery.
constexpr std::string_view kIcPvlChecksumRemedy =
    "Reset the fault of the drive. If the error comes back, run the iC-MU encoder configuration, "
    "which writes the iC-PVL configuration and reboots the iC-PVL. If it still comes back, contact "
    "support.";

constexpr IcMuStatusBit icMuStatus1Bit(uint8_t registerBit, std::string_view name,
                                       std::string_view title, std::string_view description,
                                       std::string_view remedy) {
  return {registerBit, IcHausChip::kIcMu, "STATUS1", registerBit, name, title, description, remedy};
}

constexpr IcMuStatusBit icPvlStatusBit(uint8_t registerBit, std::string_view name,
                                       std::string_view title, std::string_view description,
                                       std::string_view remedy) {
  return {static_cast<uint8_t>(8 + registerBit),
          IcHausChip::kIcPvl,
          "STATUS",
          registerBit,
          name,
          title,
          description,
          remedy};
}

constexpr IcMuStatusBit icMuStatus0Bit(uint8_t registerBit, std::string_view name,
                                       std::string_view title, std::string_view description,
                                       std::string_view remedy) {
  return {static_cast<uint8_t>(16 + registerBit),
          IcHausChip::kIcMu,
          "STATUS0",
          registerBit,
          name,
          title,
          description,
          remedy};
}

// The bits the firmware masks in (somanet_software, biss_icpvl_icmu.h: ICMU_STATUS1_MASK,
// ICPVL_STATUS_MASK, ICMU_STATUS0_MASK), with the names of the ESI. The titles are the datasheets'.
constexpr std::array<IcMuStatusBit, 17> kIcMuStatusBits = {
    icMuStatus1Bit(1, "FRQ_CNV", "Excessive signal frequency for the internal 12-bit converter",
                   "The encoder turned faster than its maximum speed.", ""),
    icMuStatus1Bit(2, "FRQ_ABZ", "Excessive signal frequency for the ABZ converter",
                   "The encoder turned faster than its maximum speed.", ""),
    icMuStatus1Bit(3, "NON_CTR",
                   "Period counter consistency error: counted period against the calculated nonius "
                   "position",
                   "The absolute position cannot be computed reliably at the current position. The "
                   "encoder is not calibrated, its mechanics changed, the ring expanded with heat, "
                   "or the magnetic ring is damaged.",
                   "Run the iC-MU calibration again."),
    icMuStatus1Bit(4, "MT_CTR",
                   "Multiturn data consistency error: counted multiturn against the external "
                   "multiturn data",
                   "The multiturn counter is not synchronised. The multiturn configuration may not "
                   "be saved.",
                   kMultiturnRemedy),
    icMuStatus1Bit(5, "MT_ERR", "Multiturn communication error",
                   "The multiturn counter is in an error state.", kMultiturnRemedy),
    icMuStatus1Bit(6, "EPR_ERR",
                   "I2C communication error: no EEPROM, or an I2C communication error",
                   "The communication with the EEPROM of the encoder failed.",
                   "Power cycle the drive. If the error stays, there is a hardware problem: "
                   "contact support."),
    icPvlStatusBit(0, "STUP_ERR", "Startup error",
                   "The iC-PVL did not start correctly, for example because the I2C line is stuck, "
                   "the EEPROM read failed, or the checksum in the EEPROM is not valid. The iC-PVL "
                   "acquires no position, and its interfaces are blocked.",
                   kIcPvlChecksumRemedy),
    icPvlStatusBit(1, "CFG_ERR", "Internal configuration error",
                   "The configuration in the RAM of the iC-PVL does not match its checksum. The "
                   "position is not valid. A write to the RAM configuration over I2C also sets "
                   "this bit, and an SCLR command clears it.",
                   kIcPvlChecksumRemedy),
    icPvlStatusBit(2, "CTR_ERR", "Internal counter error",
                   "The multiturn counter in the iC-PVL does not match its checksum. The position "
                   "is not valid. A write to MT_COUNT over I2C also sets this bit, and an SCLR "
                   "command clears it.",
                   kIcPvlChecksumRemedy),
    icPvlStatusBit(3, "POS_ERR", "Position error",
                   "The iC-PVL saw an unexpected position jump, for example from too high a speed "
                   "or acceleration of the ring, from a weak or disturbed magnetic signal, or from "
                   "a lost magnet. The position is not valid.",
                   "Check the position of the magnet, and the speed and acceleration of the axis. "
                   "Then reset the fault of the drive."),
    icPvlStatusBit(4, "BAT_ERR", "Battery error",
                   "The battery voltage is too low (iC-PVL datasheet, Elec. Char. No. 404). The "
                   "position is not valid.",
                   "Change the battery, then reset the fault of the drive."),
    icPvlStatusBit(5, "BAT_WRN", "Battery early warning",
                   "The battery voltage is low (iC-PVL datasheet, Elec. Char. No. 405).",
                   "Change the battery soon. Switch the system off, change the battery and start "
                   "again."),
    icPvlStatusBit(6, "PDR", "Power down reset detected",
                   "The iC-PVL was reset, because both the supply and the battery voltage fell far "
                   "below the battery error threshold. The counter was set to zero.",
                   "Reset the fault of the drive."),
    icPvlStatusBit(
        7, "PRESET", "Pin preset detected",
        "The iC-PVL came from a power-down, a pin preset, a sleep mode or an I2C REBOOT. Set "
        "later during operation, the bit means an unintended loss of power or a fault on the "
        "preset line.",
        "Reset the fault of the drive."),
    icMuStatus0Bit(0, "AM_MIN", "Signal error: poor level (master track)",
                   "The magnetic field is too weak for the encoder. The magnetic ring is too far "
                   "from the encoder chip or is not detected.",
                   "Check the position of the magnetic ring and the mechanical assembly."),
    icMuStatus0Bit(2, "AN_MIN", "Signal error: poor level (nonius track)",
                   "The magnetic field is too weak for the encoder. The magnetic ring is too far "
                   "from the encoder chip or is not detected.",
                   "Check the position of the magnetic ring and the mechanical assembly."),
    icMuStatus0Bit(4, "STUP", "Startup iC-MU",
                   "The encoder did not start correctly. The multiturn configuration may not be "
                   "saved.",
                   kMultiturnRemedy),
};

// The chip and the register of a bit the table does not name.
IcMuStatusBit reservedStatusBit(uint8_t bit) {
  IcMuStatusBit reserved{};
  reserved.bit = bit;
  if (bit < 8) {
    reserved.chip = IcHausChip::kIcMu;
    reserved.registerName = "STATUS1";
    reserved.registerBit = bit;
  } else if (bit < 16) {
    reserved.chip = IcHausChip::kIcPvl;
    reserved.registerName = "STATUS";
    reserved.registerBit = static_cast<uint8_t>(bit - 8);
  } else if (bit < 24) {
    reserved.chip = IcHausChip::kIcMu;
    reserved.registerName = "STATUS0";
    reserved.registerBit = static_cast<uint8_t>(bit - 16);
  }
  return reserved;
}

}  // namespace

void to_json(nlohmann::json& j, const IcMuStatusBit& bit) {
  auto text = [](std::string_view value) {
    return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(std::string(value));
  };
  j = nlohmann::json{
      {"bit", bit.bit},
      {"chip",
       bit.chip ? nlohmann::json(std::string(toString(*bit.chip))) : nlohmann::json(nullptr)},
      {"register", text(bit.registerName)},
      {"registerBit", bit.registerBit},
      {"name", text(bit.name)},
      {"title", text(bit.title)},
      {"description", text(bit.description)},
      {"remedy", text(bit.remedy)},
  };
}

std::span<const IcMuStatusBit> icMuStatusBits() { return kIcMuStatusBits; }

std::vector<IcMuStatusBit> decodeIcMuEncoderStatus(uint32_t encoderStatus) {
  std::vector<IcMuStatusBit> bits;
  for (uint8_t bit = 0; bit < 32; ++bit) {
    if ((encoderStatus & (1U << bit)) == 0) {
      continue;
    }
    const auto named = std::ranges::find(kIcMuStatusBits, bit, &IcMuStatusBit::bit);
    bits.push_back(named != kIcMuStatusBits.end() ? *named : reservedStatusBit(bit));
  }
  return bits;
}

std::expected<uint32_t, std::string> parseIcMuEncoderStatus(std::string_view text) {
  int base = 10;
  std::string_view digits = text;
  if (digits.starts_with("0x") || digits.starts_with("0X")) {
    base = 16;
    digits.remove_prefix(2);
  }
  uint32_t value = 0;
  const auto* first = std::to_address(digits.begin());
  const auto* end = std::to_address(digits.end());
  const auto [ptr, error] = std::from_chars(first, end, value, base);
  if (digits.empty() || error != std::errc() || ptr != end) {
    return std::unexpected(std::format(
        "'{}' is not an encoder status word: give a number from 0 to 4294967295, in decimal or "
        "with a 0x prefix",
        text));
  }
  return value;
}

std::optional<std::string> icMuPartError(uint8_t sensorPort, uint8_t hardRev) {
  if (isIntegratedSensorPort(sensorPort) || icMuPartName(hardRev)) {
    return std::nullopt;
  }
  return std::format(
      "register 0x74 of the encoder on external sensor port {} holds 0x{:02X}, which names no "
      "iC-MU part, so the encoder is not taken for an iC-MU",
      sensorPort, hardRev);
}

IcMuEncoderStatus icMuEncoderStatus(const EncoderConfiguration& encoder, uint8_t hardRev,
                                    uint8_t outputRegister) {
  IcMuEncoderStatus status;
  status.encoderOrdinal = encoder.encoderOrdinal;
  status.sensorPort = encoder.sensorPort;
  status.singleturnBits = encoder.singleturnBits;
  status.multiturnBits = encoder.multiturnBits;
  status.hardRev = hardRev;
  status.outputRegister = outputRegister;
  if (auto partError = icMuPartError(encoder.sensorPort, hardRev)) {
    status.reason = std::move(*partError);
  } else if (auto bitsError = icMuSingleturnBitsError(encoder.singleturnBits)) {
    status.reason = std::move(*bitsError);
  } else {
    status.masterPolePairs = icMuMasterPolePairs(encoder.singleturnBits);
    status.expectedOutputRegister = icMuOutMsb(icMuPath(encoder.sensorPort, encoder.multiturnBits),
                                               encoder.singleturnBits, encoder.multiturnBits);
  }
  status.path = icMuPath(encoder.sensorPort, encoder.multiturnBits);
  if (status.path == IcMuPath::kMultiturn) {
    for (const auto& mode : icPvlBatteryModes()) {
      if (auto acceleration = icPvlMaxAcceleration(mode, encoder.singleturnBits)) {
        status.batteryModes.push_back({.name = mode.name,
                                       .maxCurrentMicroamps = mode.maxCurrentMicroamps,
                                       .maxAccelerationKrpmPerSecond = *acceleration});
      }
    }
  }
  if (!icMuPartError(encoder.sensorPort, hardRev)) {
    status.state = icMuEncoderState(outputRegister, status.expectedOutputRegister);
  }
  return status;
}

std::span<const uint8_t> icMuRestoreRegisters() {
  static constexpr std::array<uint8_t, 33> kRegisters = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
      0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
      0x16, 0x17, 0x18, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
  };
  return kRegisters;
}

std::string icMuRestoreFileName(uint8_t sensorPort) {
  return std::format("circulo_integrated_encoder_{}_config.bin", sensorPort);
}

std::vector<uint8_t> icMuRestoreFile(std::span<const uint8_t> values) {
  const auto registers = icMuRestoreRegisters();
  std::vector<uint8_t> file;
  file.reserve(registers.size() * 2);
  for (size_t i = 0; i < registers.size() && i < values.size(); ++i) {
    file.push_back(registers[i]);
    file.push_back(values[i]);
  }
  return file;
}

void to_json(nlohmann::json& j, const IcMuRestoreEntry& entry) {
  j = nlohmann::json{{"address", entry.address},
                     {"value", entry.value},
                     {"register", entry.icMuRegister ? nlohmann::json(*entry.icMuRegister)
                                                     : nlohmann::json(nullptr)}};
}

const IcHausRegister* icMuRamRegister(uint8_t address) {
  for (const auto& space : icHausRegisterSpaces()) {
    if (space.chip != IcHausChip::kIcMu) {
      continue;
    }
    const auto r = std::ranges::find_if(space.registers, [address](const IcHausRegister& row) {
      return address >= row.address && address <= row.lastAddress;
    });
    if (r != space.registers.end()) {
      return &*r;
    }
  }
  return nullptr;
}

std::expected<std::vector<IcMuRestoreEntry>, std::string> parseIcMuRestoreFile(
    std::span<const uint8_t> bytes) {
  if (bytes.size() % 2 != 0) {
    return std::unexpected(std::format(
        "a restore file holds [address, value] pairs, and {} bytes is not a whole number of pairs",
        bytes.size()));
  }
  if (bytes.size() > kIcMuRestoreFileMaxBytes) {
    return std::unexpected(std::format("a restore file holds at most {} bytes, and this one has {}",
                                       kIcMuRestoreFileMaxBytes, bytes.size()));
  }
  std::vector<IcMuRestoreEntry> entries;
  for (size_t i = 0; i < bytes.size(); i += 2) {
    entries.push_back(
        {.address = bytes[i], .value = bytes[i + 1], .icMuRegister = icMuRamRegister(bytes[i])});
  }
  return entries;
}

IcMuFirmwareSupport icMuFirmwareSupport(std::string_view version) {
  if (version.starts_with('v')) {
    version.remove_prefix(1);
  }
  semver::version<> parsed;
  if (!semver::parse(version, parsed)) {
    return IcMuFirmwareSupport::kUnreadable;
  }

  semver::version<> jasper;
  semver::version<> configuration;
  semver::version<> calibration;
  semver::version<> calibrationEnd;
  semver::parse("10.0.0-0", jasper);
  semver::parse("5.0.0-rc.5", configuration);
  semver::parse("0.4.0", calibration);
  semver::parse("1.0.0-alpha.0", calibrationEnd);

  if (parsed >= jasper) {
    return IcMuFirmwareSupport::kJasper;
  }
  if (parsed >= configuration || (parsed >= calibration && parsed < calibrationEnd)) {
    return IcMuFirmwareSupport::kSupported;
  }
  return IcMuFirmwareSupport::kTooOld;
}

}  // namespace mm::node::somanet
