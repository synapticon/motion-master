#include "node/plant_identification.h"

#include <gtest/gtest.h>

#include <chrono>
#include <numbers>
#include <string>
#include <vector>

#include "node/somanet_drive.h"

namespace mm::node {
namespace {

using std::chrono::milliseconds;

constexpr uint32_t kRatedTorqueMilliNm = 2000;  // 2 Nm

PlantModel exampleModel() {
  PlantModel model;
  model.numerator = {0.5, 1.0};
  model.denominator = {0.001, 0.0123, 0.45};
  model.chirp = {.startFrequencyHz = 1.0,
                 .endFrequencyHz = 100.0,
                 .amplitudePermil = 300,
                 .durationMs = 3000,
                 .signalType = somanet::ChirpSignalType::kLinear};
  return model;
}

TEST(PlantIdentificationStreamDuration, AddsThePaddingAroundTheChirp) {
  auto duration = plantIdentificationStreamDuration(milliseconds(3000));
  ASSERT_TRUE(duration) << duration.error();
  EXPECT_EQ(*duration, milliseconds(3200));
}

TEST(PlantIdentificationStreamDuration, TheLongestChirpFillsTheRecordingExactly) {
  auto duration = plantIdentificationStreamDuration(milliseconds(5800));
  ASSERT_TRUE(duration) << duration.error();
  EXPECT_EQ(*duration, milliseconds(6000));
}

TEST(PlantIdentificationStreamDuration, RefusesAChirpThatDoesNotFitWithItsPadding) {
  // The drive records at most 6000 ms of system identification data, so a 5900 ms chirp alone
  // fits and with its padding does not. The message names the longest chirp that would.
  auto duration = plantIdentificationStreamDuration(milliseconds(5900));
  ASSERT_FALSE(duration);
  EXPECT_NE(duration.error().find("5800"), std::string::npos) << duration.error();
}

TEST(ConvertPlantMeasurement, RpmToRadPerSecondAndPermilToNm) {
  const std::vector<HrdSystemIdentificationSample> samples = {
      {.velocityRpm = 60.0, .torquePermil = 500},
      {.velocityRpm = -30.0, .torquePermil = -1000},
  };
  const auto measurement = convertPlantMeasurement(samples, kRatedTorqueMilliNm);
  ASSERT_EQ(measurement.torquesNm.size(), 2u);
  ASSERT_EQ(measurement.velocitiesRadPerSecond.size(), 2u);
  // 60 rpm is one turn per second, 2π rad/s. 500 per mille of 2 Nm is 1 Nm.
  EXPECT_DOUBLE_EQ(measurement.velocitiesRadPerSecond[0], 2.0 * std::numbers::pi);
  EXPECT_DOUBLE_EQ(measurement.velocitiesRadPerSecond[1], -std::numbers::pi);
  EXPECT_DOUBLE_EQ(measurement.torquesNm[0], 1.0);
  EXPECT_DOUBLE_EQ(measurement.torquesNm[1], -2.0);
}

TEST(ConvertPlantMeasurement, EmptyRecordingIsEmptyMeasurement) {
  const auto measurement = convertPlantMeasurement({}, kRatedTorqueMilliNm);
  EXPECT_TRUE(measurement.torquesNm.empty());
  EXPECT_TRUE(measurement.velocitiesRadPerSecond.empty());
}

PlantMeasurement measurementWith(double torqueNm, double velocityRadPerSecond) {
  // Alternating signs, so that the mean of the absolute values is what the thresholds see and the
  // plain mean is zero.
  PlantMeasurement measurement;
  for (int i = 0; i < 100; ++i) {
    const double sign = (i % 2 == 0) ? 1.0 : -1.0;
    measurement.torquesNm.push_back(sign * torqueNm);
    measurement.velocitiesRadPerSecond.push_back(sign * velocityRadPerSecond);
  }
  return measurement;
}

TEST(CheckPlantMeasurementAmplitudes, AGoodRecordingPassesWithoutAWarning) {
  // 0.2 Nm of 2 Nm is 100 per mille.
  auto check = checkPlantMeasurementAmplitudes(measurementWith(0.2, 20.0), kRatedTorqueMilliNm);
  ASSERT_TRUE(check) << check.error();
  EXPECT_FALSE(check->has_value());
}

TEST(CheckPlantMeasurementAmplitudes, RefusesTorqueBelowTenPermil) {
  // 0.018 Nm of 2 Nm is 9 per mille, just under the limit.
  auto check = checkPlantMeasurementAmplitudes(measurementWith(0.018, 20.0), kRatedTorqueMilliNm);
  ASSERT_FALSE(check);
  EXPECT_NE(check.error().find("torque"), std::string::npos) << check.error();
}

TEST(CheckPlantMeasurementAmplitudes, TorqueOfExactlyTenPermilPasses) {
  auto check = checkPlantMeasurementAmplitudes(measurementWith(0.02, 20.0), kRatedTorqueMilliNm);
  EXPECT_TRUE(check) << check.error();
}

TEST(CheckPlantMeasurementAmplitudes, RefusesVelocityBelowHalfARadianPerSecond) {
  auto check = checkPlantMeasurementAmplitudes(measurementWith(0.2, 0.4), kRatedTorqueMilliNm);
  ASSERT_FALSE(check);
  EXPECT_NE(check.error().find("did not move"), std::string::npos) << check.error();
}

TEST(CheckPlantMeasurementAmplitudes, WarnsOnVelocityBelowSixRadiansPerSecond) {
  auto check = checkPlantMeasurementAmplitudes(measurementWith(0.2, 3.0), kRatedTorqueMilliNm);
  ASSERT_TRUE(check) << check.error();
  ASSERT_TRUE(check->has_value());
  EXPECT_NE(check->value().find("higher amplitude"), std::string::npos) << check->value();
}

TEST(CheckPlantMeasurementAmplitudes, TheTorqueCheckComesFirst) {
  // A recording with neither torque nor motion reports the torque, because that is the cause: the
  // chirp did not run, so of course nothing moved.
  auto check = checkPlantMeasurementAmplitudes(measurementWith(0.0, 0.0), kRatedTorqueMilliNm);
  ASSERT_FALSE(check);
  EXPECT_NE(check.error().find("torque"), std::string::npos) << check.error();
}

TEST(CheckPlantMeasurementAmplitudes, AnEmptyRecordingIsRefused) {
  auto check = checkPlantMeasurementAmplitudes(PlantMeasurement{}, kRatedTorqueMilliNm);
  EXPECT_FALSE(check);
}

TEST(IsStablePlantModel, EveryNonNegativeCoefficientIsStable) {
  EXPECT_TRUE(isStablePlantModel(std::vector<double>{0.001, 0.0123, 0.45}));
  EXPECT_TRUE(isStablePlantModel(std::vector<double>{0.0, 1.0}));
  EXPECT_TRUE(isStablePlantModel(std::vector<double>{}));
}

TEST(IsStablePlantModel, OneNegativeCoefficientIsUnstable) {
  EXPECT_FALSE(isStablePlantModel(std::vector<double>{0.001, -0.0123, 0.45}));
  EXPECT_FALSE(isStablePlantModel(std::vector<double>{1.0, 1.0, -1e-9}));
}

TEST(PlantModel, InertiaAndFrictionAreTheLastTwoDenominatorCoefficients) {
  const auto model = exampleModel();
  ASSERT_TRUE(model.inertia());
  ASSERT_TRUE(model.friction());
  EXPECT_DOUBLE_EQ(*model.inertia(), 0.0123);
  EXPECT_DOUBLE_EQ(*model.friction(), 0.45);
}

TEST(PlantModel, AShortDenominatorHasNoInertia) {
  PlantModel model;
  model.denominator = {0.45};
  EXPECT_FALSE(model.inertia());
  ASSERT_TRUE(model.friction());
  EXPECT_DOUBLE_EQ(*model.friction(), 0.45);
  model.denominator.clear();
  EXPECT_FALSE(model.friction());
}

TEST(PlantModel, ToJsonCarriesTheDerivedValuesAndTheChirp) {
  const nlohmann::json j = exampleModel();
  EXPECT_EQ(j.at("numerator"), (std::vector<double>{0.5, 1.0}));
  EXPECT_DOUBLE_EQ(j.at("inertia").get<double>(), 0.0123);
  EXPECT_DOUBLE_EQ(j.at("friction").get<double>(), 0.45);
  EXPECT_EQ(j.at("chirp").at("signalType"), "linear");
  EXPECT_EQ(j.at("chirp").at("durationMs"), 3000);
  EXPECT_EQ(j.at("chirp").at("amplitudePermil"), 300);
}

TEST(PlantModelCsv, ThreeRowsWithTheDurationInSeconds) {
  EXPECT_EQ(plantModelCsv(exampleModel()), "0.5,1\n0.001,0.0123,0.45\n1,100,300,3,1\n");
}

TEST(PlantModelCsv, RoundTripsThroughTheParser) {
  const auto model = exampleModel();
  auto parsed = parsePlantModelCsv(plantModelCsv(model));
  ASSERT_TRUE(parsed) << parsed.error();
  EXPECT_EQ(parsed->numerator, model.numerator);
  EXPECT_EQ(parsed->denominator, model.denominator);
  EXPECT_DOUBLE_EQ(parsed->chirp.startFrequencyHz, 1.0);
  EXPECT_DOUBLE_EQ(parsed->chirp.endFrequencyHz, 100.0);
  EXPECT_EQ(parsed->chirp.amplitudePermil, 300u);
  EXPECT_EQ(parsed->chirp.durationMs, 3000u);
  EXPECT_EQ(parsed->chirp.signalType, somanet::ChirpSignalType::kLinear);
}

TEST(PlantModelCsv, RoundTripsCoefficientsThatNeedEveryDigit) {
  // The fit returns doubles with no particular shape. A writer that rounded them would move the
  // model's poles.
  PlantModel model;
  model.numerator = {1.0};
  model.denominator = {3.141592653589793e-05, 0.1234567890123456, 123456.78901234567};
  auto parsed = parsePlantModelCsv(plantModelCsv(model));
  ASSERT_TRUE(parsed) << parsed.error();
  EXPECT_EQ(parsed->denominator, model.denominator);
}

TEST(ParsePlantModelCsv, ReadsAFileAnOlderWriterProduced) {
  // Exponent notation from a stream with default precision, a Windows line ending, and a trailing
  // space: all of it is on drives in the field.
  const auto parsed = parsePlantModelCsv("1\r\n2.5e-05,0.00123,4.56e+01 \r\n0.5,50,200,2.5,0\r\n");
  ASSERT_TRUE(parsed) << parsed.error();
  EXPECT_EQ(parsed->numerator, (std::vector<double>{1.0}));
  EXPECT_EQ(parsed->denominator, (std::vector<double>{2.5e-05, 0.00123, 45.6}));
  EXPECT_DOUBLE_EQ(parsed->chirp.startFrequencyHz, 0.5);
  EXPECT_EQ(parsed->chirp.amplitudePermil, 200u);
  EXPECT_EQ(parsed->chirp.durationMs, 2500u);
  EXPECT_EQ(parsed->chirp.signalType, somanet::ChirpSignalType::kLogarithmic);
}

TEST(ParsePlantModelCsv, AcceptsTwoRowsWithoutAChirp) {
  const auto parsed = parsePlantModelCsv("1\n0.01,0.5");
  ASSERT_TRUE(parsed) << parsed.error();
  EXPECT_EQ(parsed->denominator, (std::vector<double>{0.01, 0.5}));
  EXPECT_EQ(parsed->chirp.durationMs, 0u);
}

TEST(ParsePlantModelCsv, RefusesOneRow) {
  const auto parsed = parsePlantModelCsv("1,2\n");
  ASSERT_FALSE(parsed);
  EXPECT_NE(parsed.error().find("denominator"), std::string::npos) << parsed.error();
}

TEST(ParsePlantModelCsv, RefusesEmptyText) { EXPECT_FALSE(parsePlantModelCsv("")); }

TEST(ParsePlantModelCsv, NamesTheFieldThatIsNotANumber) {
  const auto parsed = parsePlantModelCsv("1\n0.01,abc,0.5\n");
  ASSERT_FALSE(parsed);
  EXPECT_NE(parsed.error().find("row 2 field 2"), std::string::npos) << parsed.error();
}

TEST(ParsePlantModelCsv, RefusesAChirpRowOfTheWrongWidth) {
  const auto parsed = parsePlantModelCsv("1\n0.01,0.5\n1,100,300\n");
  ASSERT_FALSE(parsed);
  EXPECT_NE(parsed.error().find("five"), std::string::npos) << parsed.error();
}

TEST(ParsePlantModelCsv, RefusesAnUnknownSignalType) {
  const auto parsed = parsePlantModelCsv("1\n0.01,0.5\n1,100,300,3,7\n");
  ASSERT_FALSE(parsed);
  EXPECT_NE(parsed.error().find("signal type"), std::string::npos) << parsed.error();
}

}  // namespace
}  // namespace mm::node
