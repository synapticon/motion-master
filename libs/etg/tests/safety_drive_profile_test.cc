#include "etg/safety_drive_profile.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace mm::etg {
namespace {

std::array<uint8_t, kSdpSafeInputsWithProcessValues> encodeInputs(
    uint8_t statusword, uint8_t validity, int32_t position, int32_t velocity, int16_t torque) {
  std::array<uint8_t, kSdpSafeInputsWithProcessValues> inputs{};
  inputs[0] = statusword;
  inputs[1] = validity;
  std::memcpy(&inputs[2], &position, sizeof(position));
  std::memcpy(&inputs[6], &velocity, sizeof(velocity));
  std::memcpy(&inputs[10], &torque, sizeof(torque));
  return inputs;
}

TEST(SafetyDriveProfileTest, TheStoBitIsInvertedOnTheWireSoZeroIsSafe) {
  // ETG.6100.2 Table 3: the bit is set to *permit* torque. An all-zero frame — which is what a
  // fail-safe frame and a lost frame both carry — therefore requests Safe Torque Off.
  std::array<uint8_t, 8> outputs{};
  sdpEncodeControl(SdpControl{.stoRequested = true, .ss1Requested = true}, outputs);
  EXPECT_EQ(outputs[0], 0x00);

  sdpEncodeControl(SdpControl{.stoRequested = false, .ss1Requested = true}, outputs);
  EXPECT_EQ(outputs[0], 0x01);

  EXPECT_TRUE(sdpDecodeControl(std::array<uint8_t, 1>{0x00}).stoRequested);
  EXPECT_FALSE(sdpDecodeControl(std::array<uint8_t, 1>{0x01}).stoRequested);
  EXPECT_TRUE(sdpDecodeControl(std::span<const uint8_t>{}).stoRequested);
}

TEST(SafetyDriveProfileTest, TheSs1BitIsBitOneAndInvertedTheSameWay) {
  // ETG.6100.2 Table 3 bit 1 is SS1_1, with the same inversion as STO. The pairing below is the
  // one that matters in practice: releasing STO alone leaves 0x01, which still requests SS1, so a
  // drive that implements SS1 stops. Permitting motion needs 0x03.
  std::array<uint8_t, 8> outputs{};
  sdpEncodeControl(SdpControl{.stoRequested = true, .ss1Requested = true}, outputs);
  EXPECT_EQ(outputs[0], 0x00) << "all-zero requests every stop function - the fail-safe reading";

  sdpEncodeControl(SdpControl{.stoRequested = false, .ss1Requested = false}, outputs);
  EXPECT_EQ(outputs[0], 0x03) << "permitting motion clears BOTH activation bits";

  sdpEncodeControl(SdpControl{.stoRequested = true, .ss1Requested = false}, outputs);
  EXPECT_EQ(outputs[0], 0x02);

  EXPECT_TRUE(sdpDecodeControl(std::array<uint8_t, 1>{0x01}).ss1Requested);
  EXPECT_FALSE(sdpDecodeControl(std::array<uint8_t, 1>{0x03}).ss1Requested);
  EXPECT_TRUE(sdpDecodeControl(std::span<const uint8_t>{}).ss1Requested)
      << "an absent frame requests SS1 as well as STO";
}

TEST(SafetyDriveProfileTest, EncodingTheControlwordLeavesTheOtherSafeDataAlone) {
  std::array<uint8_t, 8> outputs{0xFF, 0xAA, 0xBB, 0, 0, 0, 0, 0};
  sdpEncodeControl(SdpControl{.stoRequested = false, .ss1Requested = true, .errorAcknowledge = true},
                   outputs);
  EXPECT_EQ(outputs[0], 0x81);
  EXPECT_EQ(outputs[1], 0xAA);
  EXPECT_EQ(outputs[2], 0xBB);
}

TEST(SafetyDriveProfileTest, DecodesTheSafetyStatusword) {
  EXPECT_TRUE(sdpDecodeStatus(std::array<uint8_t, 1>{0x01}).stoActive);
  EXPECT_FALSE(sdpDecodeStatus(std::array<uint8_t, 1>{0x00}).stoActive);
  EXPECT_TRUE(sdpDecodeStatus(std::array<uint8_t, 1>{0x80}).error);
  // An absent frame reads as Safe Torque Off, which is the only safe reading of no news.
  EXPECT_TRUE(sdpDecodeStatus(std::span<const uint8_t>{}).stoActive);
}

TEST(SafetyDriveProfileTest, DecodesTheSafeProcessValuesAndTheirValidity) {
  const auto inputs = encodeInputs(0x01, 0b0001'1111, 3 << 23, -1500, -250);
  const SdpProcessValues values = sdpDecodeProcessValues(inputs);

  EXPECT_EQ(values.positionFixedPoint, 3 << 23);
  EXPECT_DOUBLE_EQ(sdpPositionRevolutions(values.positionFixedPoint), 1.5);
  EXPECT_EQ(values.velocityMilliRpm, -1500);
  EXPECT_EQ(values.torqueMillinewtonMetres, -250);
  EXPECT_TRUE(values.positionValid);
  EXPECT_TRUE(values.velocityValid);
  EXPECT_TRUE(values.torqueValid);
  EXPECT_TRUE(values.crossCheckOk);
  EXPECT_TRUE(values.positionReferenced);
}

TEST(SafetyDriveProfileTest, AValidityBitIsPerValueNotPerFrame) {
  const auto inputs = encodeInputs(0x00, 1u << kSdpValidVelocityBit, 12345, 600, 7);
  const SdpProcessValues values = sdpDecodeProcessValues(inputs);

  EXPECT_TRUE(values.velocityValid);
  EXPECT_FALSE(values.positionValid);
  EXPECT_FALSE(values.torqueValid);
  // The numbers are still decoded. A caller that ignores the flags gets what the drive sent, which
  // for an invalid channel is zero — the drive encodes it that way rather than holding a stale one.
  EXPECT_EQ(values.velocityMilliRpm, 600);
}

TEST(SafetyDriveProfileTest, PositionReferencedIsStrongerThanPositionValid) {
  // A master applying an absolute limit must gate on "referenced": "valid" only says the
  // measurement is trustworthy, not that the drive knows where the axis is.
  const auto inputs = encodeInputs(0, 1u << kSdpValidPositionBit, 4242, 0, 0);
  const SdpProcessValues values = sdpDecodeProcessValues(inputs);
  EXPECT_TRUE(values.positionValid);
  EXPECT_FALSE(values.positionReferenced);
}

TEST(SafetyDriveProfileTest, AFrameTooShortForTheLayoutDecodesAsNoValuesAtAll) {
  // Not as a half-decoded number: a truncated safe position read as a real one is worse than none.
  const auto full = encodeInputs(0x01, 0xFF, 999, 999, 99);
  const SdpProcessValues values = sdpDecodeProcessValues(std::span(full).first(8));

  EXPECT_EQ(values.positionFixedPoint, 0);
  EXPECT_EQ(values.velocityMilliRpm, 0);
  EXPECT_FALSE(values.positionValid);
  EXPECT_FALSE(values.velocityValid);
  EXPECT_FALSE(values.torqueValid);
}

}  // namespace
}  // namespace mm::etg
