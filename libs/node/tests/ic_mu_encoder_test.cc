#include "node/ic_mu_encoder.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <vector>

namespace {

using mm::node::EncoderConfiguration;
using mm::node::Encoders;
using mm::node::GearRatio;
using mm::node::somanet::EncoderOrdinal;
using mm::node::somanet::EncoderShaft;
using mm::node::somanet::EncoderType;
using mm::node::somanet::IcMuConfiguration;
using mm::node::somanet::IcMuEncoderState;
using mm::node::somanet::IcMuFirmwareSupport;
using mm::node::somanet::IcMuPath;
using mm::node::somanet::IcMuRegisterWrite;
using mm::node::somanet::IcPvlCommand;
namespace somanet = mm::node::somanet;

// The rings in the field. A Circulo 7 has 19 singleturn bits, a Circulo 9 has 20, and the 18-bit
// ring sits on encoder 1 of some Circulo NG boards. The register values in each row are the
// constants a Circulo is configured with today.
struct Ring {
  uint8_t singleturnBits;
  uint8_t mpc;               // 0x0F
  uint8_t singleturnOutMsb;  // 0x11, singleturn path
  uint8_t multiturnOutMsb;   // 0x11, multiturn path with 18 multiturn bits
  uint8_t pcr;               // iC-PVL 0x02
  uint8_t masterPolePairs;
};
constexpr std::array<Ring, 3> kRings = {{
    {18, 0x04, 0x04, 0x16, 0x0F, 16},
    {19, 0x05, 0x05, 0x17, 0x1F, 32},
    {20, 0x06, 0x06, 0x18, 0x3F, 64},
}};

TEST(IcMuEncoderTest, RulesReproduceTheConstantsOfEveryRing) {
  for (const Ring& ring : kRings) {
    SCOPED_TRACE(ring.singleturnBits);
    EXPECT_EQ(somanet::icMuMasterPeriodCount(ring.singleturnBits), ring.mpc);
    EXPECT_EQ(somanet::icMuOutMsb(IcMuPath::kSingleturn, ring.singleturnBits, 18),
              ring.singleturnOutMsb);
    EXPECT_EQ(somanet::icMuOutMsb(IcMuPath::kMultiturn, ring.singleturnBits, 18),
              ring.multiturnOutMsb);
    EXPECT_EQ(somanet::icPvlPeriodCountRatio(ring.singleturnBits), ring.pcr);
    EXPECT_EQ(somanet::icMuMasterPolePairs(ring.singleturnBits), ring.masterPolePairs);
  }
}

TEST(IcMuEncoderTest, OutMsbUsesTheRealMultiturnBits) {
  EXPECT_EQ(somanet::icMuOutMsb(IcMuPath::kMultiturn, 19, 16), 0x15);
}

TEST(IcMuEncoderTest, RefusesARingTheRulesDoNotKnow) {
  EXPECT_TRUE(somanet::icMuSingleturnBitsError(17).has_value());
  EXPECT_TRUE(somanet::icMuSingleturnBitsError(21).has_value());
  EXPECT_FALSE(somanet::icMuSingleturnBitsError(19).has_value());
}

TEST(IcMuEncoderTest, MultiturnOnlyOnPortOne) {
  EXPECT_EQ(somanet::icMuPath(1, 18), IcMuPath::kMultiturn);
  EXPECT_EQ(somanet::icMuPath(1, 0), IcMuPath::kSingleturn);
  EXPECT_EQ(somanet::icMuPath(2, 18), IcMuPath::kSingleturn);
}

TEST(IcMuEncoderTest, OutputPortsFollowSafeMotion) {
  EXPECT_EQ(somanet::icMuOutputPorts(false), 0x02);
  EXPECT_EQ(somanet::icMuOutputPorts(true), 0x52);
}

Encoders roles(std::optional<EncoderOrdinal> position, std::optional<EncoderOrdinal> velocity,
               std::optional<EncoderOrdinal> commutation) {
  Encoders encoders;
  encoders.position = position;
  encoders.velocity = velocity;
  encoders.commutation = commutation;
  return encoders;
}

constexpr auto k1 = EncoderOrdinal::kEncoder1;
constexpr auto k2 = EncoderOrdinal::kEncoder2;

TEST(IcMuEncoderTest, CommutationEncoderIsOnTheMotor) {
  EXPECT_EQ(somanet::encoderShaft(roles(k2, k2, k1), k1, {101, 1}), EncoderShaft::kMotor);
}

TEST(IcMuEncoderTest, PositionEncoderBehindAGearboxIsOnTheDrivingShaft) {
  EXPECT_EQ(somanet::encoderShaft(roles(k2, k1, k1), k2, {101, 1}), EncoderShaft::kDriving);
}

TEST(IcMuEncoderTest, NoReductionMeansTheMotor) {
  EXPECT_EQ(somanet::encoderShaft(roles(k2, k1, k1), k2, {1, 1}), EncoderShaft::kMotor);
  EXPECT_EQ(somanet::encoderShaft(roles(k2, k1, k1), k2, {0, 1}), EncoderShaft::kMotor);
  EXPECT_EQ(somanet::encoderShaft(roles(k2, k1, k1), k2, {1, 0}), EncoderShaft::kMotor);
}

TEST(IcMuEncoderTest, GearRatioCountsOnlyWhenPositionIsNotOnTheCommutationEncoder) {
  // Encoder 2 serves velocity only, and position runs on the commutation encoder.
  EXPECT_EQ(somanet::encoderShaft(roles(k1, k2, k1), k2, {101, 1}), EncoderShaft::kMotor);
}

TEST(IcMuEncoderTest, VelocityDecidesWithoutAPositionEncoder) {
  EXPECT_EQ(somanet::encoderShaft(roles(std::nullopt, k2, k1), k2, {101, 1}),
            EncoderShaft::kDriving);
}

TEST(IcMuEncoderTest, EncoderWithNoRoleHasNoShaft) {
  EXPECT_FALSE(somanet::encoderShaft(roles(k1, k1, k1), k2, {101, 1}).has_value());
}

TEST(IcMuEncoderTest, DefaultBatteryModeFollowsTheShaft) {
  EXPECT_EQ(somanet::icPvlDefaultBatteryMode(EncoderShaft::kMotor), 0x5D);
  EXPECT_EQ(somanet::icPvlDefaultBatteryMode(EncoderShaft::kDriving), 0x68);
}

TEST(IcMuEncoderTest, BatteryModesAndTheirAccelerationPerRing) {
  const auto modes = somanet::icPvlBatteryModes();
  ASSERT_EQ(modes.size(), 5U);
  const std::array<uint8_t, 5> codes = {0x60, 0x58, 0x50, 0x48, 0x40};
  for (size_t i = 0; i < modes.size(); ++i) {
    EXPECT_EQ(modes[i].code, codes[i]);
  }
  EXPECT_EQ(somanet::icPvlMaxAcceleration(modes[0], 19), 3.5);
  EXPECT_EQ(somanet::icPvlMaxAcceleration(modes[0], 20), 1.7);
  EXPECT_EQ(somanet::icPvlMaxAcceleration(modes[4], 19), 895);
  EXPECT_EQ(somanet::icPvlMaxAcceleration(modes[4], 20), 447);
  EXPECT_FALSE(somanet::icPvlMaxAcceleration(modes[0], 18).has_value());
}

TEST(IcMuEncoderTest, Crc8CheckValue) {
  // The CRC of "123456789" with polynomial 0x2F, init 0x00, xorout 0xFF, not reflected.
  const std::array<uint8_t, 9> data = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(somanet::icPvlCrc8(data), 0xC1);
}

TEST(IcMuEncoderTest, Crc8OfTheReservedRegistersIsFF) {
  const std::array<uint8_t, 5> zeros{};
  EXPECT_EQ(somanet::icPvlCrc8(zeros), 0xFF);
}

std::vector<IcMuRegisterWrite> expectedRamWrites(uint8_t r0b, uint8_t r0e, uint8_t mpc, uint8_t r10,
                                                 uint8_t outMsb) {
  return {{0x00, 0x00}, {0x05, 0x88}, {0x06, 0x00}, {0x0B, r0b},    {0x0C, 0x6C}, {0x0D, 0xC0},
          {0x0E, r0e},  {0x0F, mpc},  {0x10, r10},  {0x11, outMsb}, {0x12, 0x00}, {0x13, 0xFF},
          {0x14, 0x0F}, {0x15, 0x13}, {0x16, 0x10}, {0x17, 0x02},   {0x18, 0x00}};
}

TEST(IcMuEncoderTest, SingleturnRamWritesOfACirculo7) {
  const IcMuConfiguration configuration{.path = IcMuPath::kSingleturn, .singleturnBits = 19};
  EXPECT_EQ(somanet::icMuRamWrites(configuration), expectedRamWrites(0x02, 0x04, 0x05, 0x00, 0x05));
}

TEST(IcMuEncoderTest, MultiturnRamWritesOfASafeMotionCirculo9) {
  const IcMuConfiguration configuration{
      .path = IcMuPath::kMultiturn, .singleturnBits = 20, .multiturnBits = 18, .safeMotion = true};
  EXPECT_EQ(somanet::icMuRamWrites(configuration), expectedRamWrites(0x52, 0x44, 0x06, 0x6F, 0x18));
}

std::vector<IcMuRegisterWrite> expectedEepromWrites(uint8_t pcr, uint8_t battery, uint8_t crc) {
  return {{0x5E, 0xA0}, {0x5C, 0x60},    {0x5B, 0x40}, {0x5D, 0x6C}, {0x60, 0x28}, {0x61, 0x09},
          {0x62, pcr},  {0x63, battery}, {0x64, 0x50}, {0x65, 0x30}, {0x66, crc},  {0x67, 0x00},
          {0x68, 0x00}, {0x69, 0x00},    {0x6A, 0x00}, {0x6B, 0x00}, {0x6C, 0xFF}, {0x75, 0x0A}};
}

TEST(IcMuEncoderTest, IcPvlEepromWritesCarryTheCrcOfEachRingAndShaft) {
  struct Case {
    uint8_t singleturnBits;
    uint8_t pcr;
    uint8_t battery;
    uint8_t crc;
  };
  // The CRCs come from a separate CRC library with the same parameters, not from icPvlCrc8.
  const std::array<Case, 6> cases = {{
      {19, 0x1F, 0x5D, 0xD2},
      {19, 0x1F, 0x68, 0xEB},
      {20, 0x3F, 0x5D, 0x3E},
      {20, 0x3F, 0x68, 0x07},
      {18, 0x0F, 0x5D, 0xA4},
      {18, 0x0F, 0x68, 0x9D},
  }};
  for (const Case& c : cases) {
    SCOPED_TRACE(testing::Message() << int{c.singleturnBits} << " " << int{c.battery});
    const IcMuConfiguration configuration{.path = IcMuPath::kMultiturn,
                                          .singleturnBits = c.singleturnBits,
                                          .multiturnBits = 18,
                                          .batteryMode = c.battery};
    EXPECT_EQ(somanet::icPvlEepromWrites(configuration),
              expectedEepromWrites(c.pcr, c.battery, c.crc));
  }
}

TEST(IcMuEncoderTest, IcPvlCommandsAndRegisterAccess) {
  const std::vector<IcMuRegisterWrite> reboot = {{0x5E, 0xC2}, {0x5C, 0x60}, {0x5B, 0x11},
                                                 {0x5D, 0x60}, {0x60, 0x03}, {0x75, 0x0A}};
  EXPECT_EQ(somanet::icPvlCommand(IcPvlCommand::kReboot), reboot);

  const std::vector<IcMuRegisterWrite> sclr = {{0x5E, 0xC2}, {0x5C, 0x60}, {0x5B, 0x11},
                                               {0x5D, 0x60}, {0x60, 0x05}, {0x75, 0x0A}};
  EXPECT_EQ(somanet::icPvlCommand(IcPvlCommand::kSclr), sclr);

  const std::vector<IcMuRegisterWrite> positionOn = {{0x5E, 0xC2}, {0x5C, 0x60}, {0x5B, 0x05},
                                                     {0x5D, 0x60}, {0x60, 0xB0}, {0x75, 0x0A}};
  EXPECT_EQ(somanet::icPvlRegisterWrite(0x05, somanet::kIcPvlPositionOutputOn), positionOn);

  const std::vector<IcMuRegisterWrite> readSync = {
      {0x5E, 0xC3}, {0x5C, 0x60}, {0x5B, 0x0D}, {0x5D, 0x60}, {0x75, 0x0A}};
  EXPECT_EQ(somanet::icPvlRegisterRead(somanet::kIcPvlSyncRegister), readSync);
}

TEST(IcMuEncoderTest, SpoMtIsTheRoundedOffsetBetweenTheTwoAngles) {
  // Half a revolution on a 19-bit ring is 1800 tenths of a degree. SYNC 2 is 900. The offset of
  // 900 is four steps of 225.
  EXPECT_EQ(somanet::icMuSpoMtRegister(1U << 18, 19, 2), 0x45);
  // Only the singleturn bits of the raw position count.
  EXPECT_EQ(somanet::icMuSpoMtRegister((7U << 19) | (1U << 18), 19, 2), 0x45);
  // Only the SYNC bits of the iC-PVL register count.
  EXPECT_EQ(somanet::icMuSpoMtRegister(1U << 18, 19, 0xFA), 0x45);
}

TEST(IcMuEncoderTest, SpoMtRoundsAboveHalfAStepOnly) {
  // 1 tenth below the halfway point of a step, then 1 above it, on a 20-bit ring with SYNC 0.
  // 112 tenths is 112 * 2^20 / 3600 counts, rounded up so the integer angle reaches 112.
  const uint32_t countsFor112 = (112U << 20) / 3600 + 1;
  const uint32_t countsFor113 = (113U << 20) / 3600 + 1;
  EXPECT_EQ(somanet::icMuSpoMtRegister(countsFor112, 20, 0), 0x06);
  EXPECT_EQ(somanet::icMuSpoMtRegister(countsFor113, 20, 0), 0x16);
}

TEST(IcMuEncoderTest, SpoMtWrapsAWholeRevolutionToZero) {
  // The iC-MU angle is just below a whole revolution, and SYNC 0: the offset rounds to 16 steps.
  EXPECT_EQ(somanet::icMuSpoMtRegister((1U << 19) - 1, 19, 0), 0x05);
}

TEST(IcMuEncoderTest, SpoMtWhenTheIcPvlIsAhead) {
  // iC-MU at 0, SYNC 1 at 450: the offset is 3150, fourteen steps.
  EXPECT_EQ(somanet::icMuSpoMtRegister(0, 18, 1), 0xE4);
}

TEST(IcMuEncoderTest, FirmwareSupport) {
  EXPECT_EQ(somanet::icMuFirmwareSupport("v5.0.0-rc.5"), IcMuFirmwareSupport::kSupported);
  EXPECT_EQ(somanet::icMuFirmwareSupport("5.3.1"), IcMuFirmwareSupport::kSupported);
  EXPECT_EQ(somanet::icMuFirmwareSupport("v5.0.0-rc.4"), IcMuFirmwareSupport::kTooOld);
  EXPECT_EQ(somanet::icMuFirmwareSupport("v4.4.1"), IcMuFirmwareSupport::kTooOld);
  EXPECT_EQ(somanet::icMuFirmwareSupport("v0.3.9"), IcMuFirmwareSupport::kTooOld);
  EXPECT_EQ(somanet::icMuFirmwareSupport("v0.4.2"), IcMuFirmwareSupport::kSupported);
  EXPECT_EQ(somanet::icMuFirmwareSupport("v10.0.0-jasper.5"), IcMuFirmwareSupport::kJasper);
  EXPECT_EQ(somanet::icMuFirmwareSupport("not a version"), IcMuFirmwareSupport::kUnreadable);
}

EncoderConfiguration icMu(uint8_t sensorPort, uint8_t singleturnBits, uint8_t multiturnBits) {
  EncoderConfiguration encoder;
  encoder.encoder = sensorPort == 2 ? EncoderOrdinal::kEncoder2 : EncoderOrdinal::kEncoder1;
  encoder.sensorPort = sensorPort;
  encoder.type = EncoderType::kInternal;
  encoder.singleturnBits = singleturnBits;
  encoder.multiturnBits = multiturnBits;
  return encoder;
}

TEST(IcMuEncoderTest, ListsBissAndInternalEncodersOnPortsOneAndTwo) {
  auto encoder = icMu(1, 19, 0);
  EXPECT_TRUE(somanet::isIcMuEncoder(encoder));
  encoder.type = EncoderType::kBiss;
  EXPECT_TRUE(somanet::isIcMuEncoder(encoder));
  encoder.sensorPort = 2;
  EXPECT_TRUE(somanet::isIcMuEncoder(encoder));
  encoder.sensorPort = 3;
  EXPECT_FALSE(somanet::isIcMuEncoder(encoder));
  encoder.sensorPort = 0;
  EXPECT_FALSE(somanet::isIcMuEncoder(encoder));
  encoder.sensorPort = 1;
  encoder.type = EncoderType::kIncremental;
  EXPECT_FALSE(somanet::isIcMuEncoder(encoder));
  encoder.type = EncoderType::kNone;
  EXPECT_FALSE(somanet::isIcMuEncoder(encoder));
}

TEST(IcMuEncoderTest, DoesNotListAnEncoderWithoutSingleturnBits) {
  EXPECT_FALSE(somanet::isIcMuEncoder(icMu(1, 0, 0)));
}

TEST(IcMuEncoderTest, StateComparesTheOutputRegister) {
  EXPECT_EQ(somanet::icMuEncoderState(0xA5, 0x05), IcMuEncoderState::kNotConfigured);
  EXPECT_EQ(somanet::icMuEncoderState(0x05, 0x05), IcMuEncoderState::kConfigured);
  EXPECT_EQ(somanet::icMuEncoderState(0x17, 0x05), IcMuEncoderState::kInvalid);
}

TEST(IcMuEncoderTest, StateWithoutAnExpectedValueIsOnlyNotConfigured) {
  EXPECT_EQ(somanet::icMuEncoderState(0xA5, std::nullopt), IcMuEncoderState::kNotConfigured);
  EXPECT_EQ(somanet::icMuEncoderState(0x05, std::nullopt), std::nullopt);
}

TEST(IcMuEncoderTest, PartNameFromHardRev) {
  // The three codes of Table 92.
  EXPECT_EQ(somanet::icMuPartName(0x0A), "iC-MU128");
  EXPECT_EQ(somanet::icMuPartName(0x15), "iC-MU150");
  EXPECT_EQ(somanet::icMuPartName(0x24), "iC-MU200");
  EXPECT_EQ(somanet::icMuPartName(0x30), std::nullopt);
}

TEST(IcMuEncoderTest, StatusExpectsTheRulesOfEveryRing) {
  for (const Ring& ring : kRings) {
    SCOPED_TRACE(ring.singleturnBits);
    auto singleturn =
        somanet::icMuEncoderStatus(icMu(2, ring.singleturnBits, 0), 0x15, ring.singleturnOutMsb);
    EXPECT_EQ(singleturn.masterPolePairs, ring.masterPolePairs);
    EXPECT_EQ(singleturn.expectedOutputRegister, ring.singleturnOutMsb);
    EXPECT_EQ(singleturn.state, IcMuEncoderState::kConfigured);
    EXPECT_EQ(singleturn.reason, std::nullopt);

    auto multiturn =
        somanet::icMuEncoderStatus(icMu(1, ring.singleturnBits, 18), 0x15, ring.singleturnOutMsb);
    EXPECT_EQ(multiturn.expectedOutputRegister, ring.multiturnOutMsb);
    EXPECT_EQ(multiturn.state, IcMuEncoderState::kInvalid);
  }
}

TEST(IcMuEncoderTest, StatusOnPortTwoIgnoresTheMultiturnBits) {
  auto status = somanet::icMuEncoderStatus(icMu(2, 19, 18), 0x15, 0x05);
  EXPECT_EQ(status.expectedOutputRegister, 0x05);
  EXPECT_EQ(status.state, IcMuEncoderState::kConfigured);
}

TEST(IcMuEncoderTest, StatusOfAnUnknownRingSaysWhy) {
  auto status = somanet::icMuEncoderStatus(icMu(1, 17, 0), 0x15, 0x03);
  EXPECT_EQ(status.masterPolePairs, std::nullopt);
  EXPECT_EQ(status.expectedOutputRegister, std::nullopt);
  EXPECT_EQ(status.state, std::nullopt);
  ASSERT_TRUE(status.reason);
  EXPECT_FALSE(status.reason->empty());
}

TEST(IcMuEncoderTest, StatusJson) {
  const nlohmann::json j = somanet::icMuEncoderStatus(icMu(1, 20, 0), 0x24, 0xA5);
  EXPECT_EQ(j["encoder"], 1);
  EXPECT_EQ(j["sensorPort"], 1);
  EXPECT_EQ(j["singleturnBits"], 20);
  EXPECT_EQ(j["multiturnBits"], 0);
  EXPECT_EQ(j["masterPolePairs"], 64);
  EXPECT_EQ(j["hardRev"], 0x24);
  EXPECT_EQ(j["part"], "iC-MU200");
  EXPECT_EQ(j["revision"], 4);
  EXPECT_EQ(j["outputRegister"], 0xA5);
  EXPECT_EQ(j["expectedOutputRegister"], 0x06);
  EXPECT_EQ(j["state"], "not-configured");
  EXPECT_TRUE(j["reason"].is_null());
}

TEST(IcMuEncoderTest, StatusOffersTheBatteryModesOfTheRingOnTheMultiturnPath) {
  auto multiturn = somanet::icMuEncoderStatus(icMu(1, 19, 18), 0x15, 0x17);
  EXPECT_EQ(multiturn.path, IcMuPath::kMultiturn);
  ASSERT_EQ(multiturn.batteryModes.size(), somanet::icPvlBatteryModes().size());
  EXPECT_EQ(multiturn.batteryModes[0].name, "6-microamperes");
  EXPECT_EQ(multiturn.batteryModes[0].maxAccelerationKrpmPerSecond, 3.5);

  // 16 master pole pairs offer no mode, and the singleturn path takes none.
  EXPECT_TRUE(somanet::icMuEncoderStatus(icMu(1, 18, 18), 0x15, 0x16).batteryModes.empty());
  EXPECT_TRUE(somanet::icMuEncoderStatus(icMu(2, 19, 0), 0x15, 0x05).batteryModes.empty());
}

TEST(IcMuEncoderTest, RestoreFileRoundTrips) {
  const std::vector<uint8_t> values(somanet::icMuRestoreRegisters().size(), 0x42);
  const auto file = somanet::icMuRestoreFile(values);
  ASSERT_EQ(file.size(), somanet::kIcMuRestoreFileMaxBytes);
  auto entries = somanet::parseIcMuRestoreFile(file);
  ASSERT_TRUE(entries) << entries.error();
  ASSERT_EQ(entries->size(), 33u);
  EXPECT_EQ((*entries)[0x0C].address, 0x0C);
  EXPECT_EQ((*entries)[0x0C].value, 0x42);
  ASSERT_NE((*entries)[0x0C].icMuRegister, nullptr);
  EXPECT_EQ((*entries)[0x0C].icMuRegister->namedFields()[0].name, "CFGEW");
  EXPECT_EQ(entries->back().address, 0x59);
  EXPECT_NE(entries->back().icMuRegister, nullptr);
  EXPECT_EQ(somanet::icMuRestoreFileName(2), "circulo_integrated_encoder_2_config.bin");
}

TEST(IcMuEncoderTest, RestoreFileRefusesWhatTheDriveRefuses) {
  EXPECT_FALSE(somanet::parseIcMuRestoreFile(std::vector<uint8_t>{0x00, 0x01, 0x02}));
  EXPECT_FALSE(somanet::parseIcMuRestoreFile(std::vector<uint8_t>(68, 0)));
  EXPECT_TRUE(somanet::parseIcMuRestoreFile(std::vector<uint8_t>{}));
}

}  // namespace
