#include "node/ic_mu_calibration_engine.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <vector>

namespace {

using mm::node::HrdEncoderSample;
using mm::node::somanet::IcMuAnalogAdjustments;
using mm::node::somanet::IcMuCalibrationEngine;
using mm::node::somanet::IcMuNoniusOffsetTable;
using mm::node::somanet::IcMuTrack;

// A recording of an ideal ring: the master track has one period per pole pair and the nonius track
// one fewer, each reported as a 14-bit angle within its period.
std::vector<HrdEncoderSample> idealRecording(uint8_t masterPolePairs, double revolutions,
                                             size_t count) {
  std::vector<HrdEncoderSample> samples(count);
  for (size_t i = 0; i < count; ++i) {
    const double turn = revolutions * static_cast<double>(i) / static_cast<double>(count);
    auto counts = [turn](double periods) {
      const double fraction = (turn * periods) - std::floor(turn * periods);
      return static_cast<uint16_t>(static_cast<unsigned>(fraction * 16384.0) & 0x3FFFU);
    };
    samples[i].masterCount = counts(masterPolePairs);
    samples[i].noniusCount = counts(masterPolePairs - 1.0);
    samples[i].raw = samples[i].masterCount | (uint32_t{samples[i].noniusCount} << 14);
  }
  return samples;
}

TEST(IcMuCalibrationEngineTest, CreatesACalibrationForAnIcMuPart) {
  EXPECT_TRUE(IcMuCalibrationEngine::create(0x15, 32));
  EXPECT_TRUE(IcMuCalibrationEngine::create(0x24, 64));
}

TEST(IcMuCalibrationEngineTest, KeepsTheAdjustmentsItIsGiven) {
  auto engine = IcMuCalibrationEngine::create(0x15, 32);
  ASSERT_TRUE(engine) << engine.error();
  const IcMuAnalogAdjustments master{
      .gain = 3, .sineOffset = 4, .cosineOffset = 5, .phase = {.phase = 6, .phaseRange = 0}};
  const IcMuAnalogAdjustments nonius{
      .gain = 7, .sineOffset = 8, .cosineOffset = 9, .phase = {.phase = 0x3F, .phaseRange = 0}};
  ASSERT_TRUE(engine->setAdjustments(master, nonius));
  EXPECT_EQ(engine->adjustments(IcMuTrack::kMaster), master);
  EXPECT_EQ(engine->adjustments(IcMuTrack::kNonius), nonius);
  ASSERT_TRUE(engine->setNoniusTable(IcMuNoniusOffsetTable{}));
}

TEST(IcMuCalibrationEngineTest, AdjustNeedsAnAnalysis) {
  auto engine = IcMuCalibrationEngine::create(0x15, 32);
  ASSERT_TRUE(engine) << engine.error();
  EXPECT_FALSE(engine->adjust(true, true));
  EXPECT_FALSE(engine->optimizeNoniusTable());
}

TEST(IcMuCalibrationEngineTest, AnalysesAnIdealRecording) {
  auto engine = IcMuCalibrationEngine::create(0x15, 32);
  ASSERT_TRUE(engine) << engine.error();
  ASSERT_TRUE(engine->setAdjustments({}, {}));
  const auto samples = idealRecording(32, 1.1, 6000);
  auto analysis = engine->analyze(samples);
  ASSERT_TRUE(analysis) << analysis.error();
  EXPECT_NEAR(analysis->revolutions, 1.1, 0.05);

  // An ideal recording leaves no error on either track.
  for (const auto& track : {analysis->master, analysis->nonius}) {
    EXPECT_NEAR(track.sineOffset, 0, 0.01);
    EXPECT_NEAR(track.cosineOffset, 0, 0.01);
    EXPECT_NEAR(track.gain, 0, 0.01);
    EXPECT_NEAR(track.phase, 0, 0.01);
  }
  EXPECT_TRUE(analysis->noniusValid);
  EXPECT_FALSE(analysis->phaseError.empty());
  EXPECT_EQ(analysis->phaseError.size(), analysis->phaseMargin.size());

  EXPECT_TRUE(engine->adjust(true, false));
  EXPECT_TRUE(engine->optimizeNoniusTable());
}

TEST(IcMuCalibrationEngineTest, AdjustKeepsATrackThatIsNotNamed) {
  auto engine = IcMuCalibrationEngine::create(0x15, 32);
  ASSERT_TRUE(engine) << engine.error();
  const IcMuAnalogAdjustments nonius{
      .gain = 7, .sineOffset = 8, .cosineOffset = 9, .phase = {.phase = 0x3F, .phaseRange = 0}};
  ASSERT_TRUE(engine->setAdjustments({}, nonius));
  ASSERT_TRUE(engine->analyze(idealRecording(32, 1.1, 6000)));
  ASSERT_TRUE(engine->adjust(true, false));
  EXPECT_EQ(engine->adjustments(IcMuTrack::kNonius), nonius);
}

}  // namespace

namespace {

// A recording whose nonius track has its cosine scaled by @p cosineScale, which the analysis
// answers with a correction of the nonius gain.
std::vector<HrdEncoderSample> gainErrorRecording(double cosineScale) {
  constexpr uint8_t kPolePairs = 32;
  constexpr size_t kCount = 6000;
  constexpr double kRevolutions = 1.1;
  std::vector<HrdEncoderSample> samples(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    const double turn = kRevolutions * static_cast<double>(i) / static_cast<double>(kCount);
    auto ideal = [turn](double periods) { return (turn * periods) - std::floor(turn * periods); };
    auto counts = [](double fraction) {
      return static_cast<uint16_t>(static_cast<unsigned>(fraction * 16384.0) & 0x3FFFU);
    };
    const double phi = 2 * std::numbers::pi * ideal(kPolePairs - 1.0);
    double distorted = std::atan2(std::sin(phi), cosineScale * std::cos(phi)) / (2 * std::numbers::pi);
    if (distorted < 0) {
      distorted += 1;
    }
    samples[i].masterCount = counts(ideal(kPolePairs));
    samples[i].noniusCount = counts(distorted);
    samples[i].raw = samples[i].masterCount | (uint32_t{samples[i].noniusCount} << 14);
  }
  return samples;
}

// The nonius gain after one adjustment from @p start, or nothing when MU_3SL refuses it.
std::optional<uint8_t> adjustedNoniusGain(double cosineScale, uint8_t start) {
  auto engine = IcMuCalibrationEngine::create(0x15, 32);
  EXPECT_TRUE(engine);
  IcMuAnalogAdjustments master{};
  IcMuAnalogAdjustments nonius{};
  nonius.gain = start;
  EXPECT_TRUE(engine->setAdjustments(master, nonius));
  auto analysis = engine->analyze(gainErrorRecording(cosineScale));
  EXPECT_TRUE(analysis) << analysis.error();
  if (!engine->adjust(false, true)) {
    return std::nullopt;
  }
  return engine->adjustments(IcMuTrack::kNonius).gain;
}

// GX is a signed 7-bit value: 0x00 is a gain of 1.000, 0x3F the highest and 0x40 the lowest (iC-MU
// datasheet Rev B1, Table 10). So 0x7F, -1, moving by +4 to 0x03 is a correct step across 1.000,
// not a wrap. A cosine at 97 % asks for about +21 LSB, and one at 103 % for about -21.
TEST(IcMuCalibrationEngineTest, MovesTheGainAcrossZeroAndRefusesToPassItsEnds) {
  EXPECT_EQ(adjustedNoniusGain(0.97, 0x7F), 0x14);  // -1 + 21 = +20
  EXPECT_EQ(adjustedNoniusGain(0.97, 0x50), 0x65);  // -48 + 21 = -27
  EXPECT_EQ(adjustedNoniusGain(1.03, 0x00), 0x6B);  // 0 - 21 = -21
  // +60 + 21 passes +63, and -48 - 21 passes -64. MU_3SL refuses both. Its message names the
  // master track although the nonius gain is the one that cannot move.
  EXPECT_EQ(adjustedNoniusGain(0.97, 0x3C), std::nullopt);
  EXPECT_EQ(adjustedNoniusGain(1.03, 0x50), std::nullopt);
}

}  // namespace
