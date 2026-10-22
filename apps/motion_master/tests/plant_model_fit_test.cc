#include "plant_model_fit.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "auto_tuning/client.h"
#include "node/plant_identification.h"

namespace {

using mm::identifyPlantModelRequest;
using mm::makePlantModelFit;
using mm::parseIdentifyPlantModelReply;
using mm::plantMeasurementCsv;
using mm::node::PlantMeasurement;

PlantMeasurement threeSamples() {
  return PlantMeasurement{.torquesNm = {0.1, -0.2, 0.3},
                          .velocitiesRadPerSecond = {1.5, -2.5, 3.5}};
}

TEST(PlantMeasurementCsv, OneRowPerMillisecondWithNoHeader) {
  // The program assumes 1000 Hz and reads the time column, so the index must become seconds and
  // nothing may precede the first row.
  EXPECT_EQ(plantMeasurementCsv(threeSamples()), "0,0.1,1.5\n0.001,-0.2,-2.5\n0.002,0.3,3.5\n");
}

TEST(PlantMeasurementCsv, EmptyMeasurementIsEmptyText) {
  EXPECT_EQ(plantMeasurementCsv(PlantMeasurement{}), "");
}

TEST(IdentifyPlantModelRequest, NamesTheFunctionAndCarriesTheMeasurementInline) {
  const auto request = nlohmann::json::parse(identifyPlantModelRequest(threeSamples(), 1.0, 100.0));
  EXPECT_EQ(request.at("run"), "identify_plant_model");
  EXPECT_EQ(request.at("data").at("f0"), 1.0);
  EXPECT_EQ(request.at("data").at("f1"), 100.0);
  EXPECT_EQ(request.at("data").at("csv"), plantMeasurementCsv(threeSamples()));
}

TEST(ParseIdentifyPlantModelReply, ReadsTheModelAndCarriesTheBodeThrough) {
  const std::string body = R"({"numerators":[2.0],"denominators":[1.0,3.0],)"
                           R"("bode":{"Frequency_Hz":[0.5,1.0]},"duration":0.2})";
  const auto fit = parseIdentifyPlantModelReply(200, body);
  ASSERT_TRUE(fit) << fit.error();
  EXPECT_EQ(fit->numerator, (std::vector<double>{2.0}));
  EXPECT_EQ(fit->denominator, (std::vector<double>{1.0, 3.0}));
  EXPECT_EQ(fit->bode.at("Frequency_Hz"), (std::vector<double>{0.5, 1.0}));
}

TEST(ParseIdentifyPlantModelReply, MissingBodeIsAnEmptyObject) {
  const auto fit = parseIdentifyPlantModelReply(200, R"({"numerators":[1],"denominators":[1]})");
  ASSERT_TRUE(fit) << fit.error();
  EXPECT_TRUE(fit->bode.is_object());
  EXPECT_TRUE(fit->bode.empty());
}

TEST(ParseIdentifyPlantModelReply, ARefusedInputArrivesAsStatus200WithAnError) {
  // The program's own convention: a bad input is a 200 with an `error` property, so the status
  // alone would read it as success.
  const auto fit = parseIdentifyPlantModelReply(200, R"({"error":"f1 must exceed f0"})");
  ASSERT_FALSE(fit);
  EXPECT_NE(fit.error().find("f1 must exceed f0"), std::string::npos);
}

TEST(ParseIdentifyPlantModelReply, ANon200StatusIsRefusedWithTheStatus) {
  const auto fit = parseIdentifyPlantModelReply(500, "Traceback");
  ASSERT_FALSE(fit);
  EXPECT_NE(fit.error().find("500"), std::string::npos);
}

TEST(ParseIdentifyPlantModelReply, AReplyWithoutBothCoefficientArraysIsRefused) {
  const auto fit = parseIdentifyPlantModelReply(200, R"({"numerators":[1]})");
  ASSERT_FALSE(fit);
  EXPECT_NE(fit.error().find("denominators"), std::string::npos);
}

TEST(ParseIdentifyPlantModelReply, NonJsonIsRefused) {
  const auto fit = parseIdentifyPlantModelReply(200, "<html>");
  ASSERT_FALSE(fit);
}

TEST(MakePlantModelFit, WithoutAClientEveryCallNamesTheMissingProgram) {
  const std::optional<mm::auto_tuning::Client> none;
  const auto fit = makePlantModelFit(none);
  const auto result = fit(threeSamples(), 1.0, 100.0);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("auto-tuning program is not running"), std::string::npos);
}

}  // namespace
