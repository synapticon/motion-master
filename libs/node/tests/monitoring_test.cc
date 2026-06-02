#include "node/monitoring.h"

#include <gtest/gtest.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>

namespace {

using mm::node::MonitoredParameter;
using mm::node::Monitoring;

TEST(MonitoringToJson, SerialisesAllFields) {
  Monitoring m;
  m.topic = "left-leg";
  m.name = "Left Leg";
  m.interval = std::chrono::milliseconds{1000};
  m.bufferSize = 16;
  m.parameters = {MonitoredParameter{1, 0x2030, 1}, MonitoredParameter{1, 0x6064, 0}};

  const nlohmann::json j = m;

  EXPECT_EQ(j.at("topic"), "left-leg");
  EXPECT_EQ(j.at("name"), "Left Leg");
  EXPECT_EQ(j.at("interval"), 1000);
  EXPECT_EQ(j.at("bufferSize"), 16);

  ASSERT_EQ(j.at("parameters").size(), 2u);
  EXPECT_EQ(j["parameters"][0]["devicePosition"], 1);
  EXPECT_EQ(j["parameters"][0]["index"], 0x2030);
  EXPECT_EQ(j["parameters"][0]["subindex"], 1);
  EXPECT_EQ(j["parameters"][1]["index"], 0x6064);
  EXPECT_EQ(j["parameters"][1]["subindex"], 0);
}

TEST(MonitoringToJson, OmitsNameWhenUnset) {
  Monitoring m;
  m.topic = "t";
  m.name = std::nullopt;
  m.interval = std::chrono::milliseconds{50};
  m.bufferSize = 32;
  m.parameters = {MonitoredParameter{2, 0x6041, 0}};

  const nlohmann::json j = m;

  EXPECT_FALSE(j.contains("name"));
  EXPECT_EQ(j.at("interval"), 50);
  EXPECT_EQ(j.at("bufferSize"), 32);
  ASSERT_EQ(j.at("parameters").size(), 1u);
  EXPECT_EQ(j["parameters"][0]["devicePosition"], 2);
}

}  // namespace
