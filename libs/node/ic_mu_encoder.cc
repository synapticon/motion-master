#include "node/ic_mu_encoder.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <semver.hpp>
#include <string>
#include <vector>

namespace mm::node::somanet {

namespace {

// The exchange registers 0x60-0x6F hold one transfer; 0x5B-0x5E say where it goes.
constexpr uint8_t kI2cDeviceStart = 0x5B;
constexpr uint8_t kI2cRamStart = 0x5C;
constexpr uint8_t kI2cRamEnd = 0x5D;
constexpr uint8_t kI2cDeviceId = 0x5E;

// I²C device ids, the R/W bit included (iC-MU datasheet, Table 103; iC-PVL datasheet, Table 6).
constexpr uint8_t kWriteEeprom = 0xA0;
constexpr uint8_t kWriteIcPvl = 0xC2;
constexpr uint8_t kReadIcPvl = 0xC3;

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

// The I²C transfer of one byte between the exchange register and @p deviceRegister of @p deviceId.
std::vector<IcMuRegisterWrite> oneByteTransfer(uint8_t deviceId, uint8_t deviceRegister) {
  return {{kI2cDeviceId, deviceId},
          {kI2cRamStart, kIcMuExchangeRegister},
          {kI2cDeviceStart, deviceRegister},
          {kI2cRamEnd, kIcMuExchangeRegister}};
}

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
      // Masks the AN_MAX error.
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

  const auto lastExchangeRegister = static_cast<uint8_t>(kIcMuExchangeRegister + eeprom.size() - 1);
  std::vector<IcMuRegisterWrite> writes = {{kI2cDeviceId, kWriteEeprom},
                                           {kI2cRamStart, kIcMuExchangeRegister},
                                           {kI2cDeviceStart, kIcPvlEepromStart},
                                           {kI2cRamEnd, lastExchangeRegister}};
  std::ranges::transform(eeprom, std::back_inserter(writes),
                         [address = kIcMuExchangeRegister](uint8_t value) mutable {
                           return IcMuRegisterWrite{address++, value};
                         });
  writes.push_back(command(IcMuCommand::kI2cCom));
  return writes;
}

std::vector<IcMuRegisterWrite> icPvlRegisterWrite(uint8_t icPvlRegister, uint8_t value) {
  auto writes = oneByteTransfer(kWriteIcPvl, icPvlRegister);
  writes.push_back({kIcMuExchangeRegister, value});
  writes.push_back(command(IcMuCommand::kI2cCom));
  return writes;
}

std::vector<IcMuRegisterWrite> icPvlCommand(IcPvlCommand c) {
  return icPvlRegisterWrite(kIcPvlCommandRegister, static_cast<uint8_t>(c));
}

std::vector<IcMuRegisterWrite> icPvlRegisterRead(uint8_t icPvlRegister) {
  auto writes = oneByteTransfer(kReadIcPvl, icPvlRegister);
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
