#include "node/ic_mu_calibration_engine.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
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
