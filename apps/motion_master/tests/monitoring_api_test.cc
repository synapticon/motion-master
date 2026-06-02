#include "monitoring_api.h"

#include <gtest/gtest.h>

#include <chrono>
#include <nlohmann/json.hpp>

namespace {

using mm::parseMonitoringRequest;
using mm::parseWsCommand;
using mm::WsCommand;

TEST(ParseWsCommand, ParsesSubscribe) {
  auto cmd = parseWsCommand(R"({"subscribe":"left-leg"})");
  ASSERT_TRUE(cmd.has_value());
  EXPECT_EQ(cmd->action, WsCommand::Action::Subscribe);
  EXPECT_EQ(cmd->topic, "left-leg");
}

TEST(ParseWsCommand, ParsesUnsubscribe) {
  auto cmd = parseWsCommand(R"({"unsubscribe":"left-leg"})");
  ASSERT_TRUE(cmd.has_value());
  EXPECT_EQ(cmd->action, WsCommand::Action::Unsubscribe);
  EXPECT_EQ(cmd->topic, "left-leg");
}

TEST(ParseWsCommand, RejectsMalformedOrUnknown) {
  EXPECT_FALSE(parseWsCommand("not json").has_value());
  EXPECT_FALSE(parseWsCommand("[]").has_value());                           // not an object
  EXPECT_FALSE(parseWsCommand("\"left-leg\"").has_value());                 // not an object
  EXPECT_FALSE(parseWsCommand(R"({"watch":"left-leg"})").has_value());      // unknown key
  EXPECT_FALSE(parseWsCommand(R"({"subscribe":42})").has_value());          // non-string topic
  EXPECT_FALSE(parseWsCommand(R"({"subscribe":"left/leg"})").has_value());  // not URL-safe
  EXPECT_FALSE(parseWsCommand(R"({"subscribe":""})").has_value());          // empty topic
}

TEST(ParseMonitoringRequest, ParsesFullRequest) {
  const auto body = nlohmann::json::parse(R"({
    "topic": "left-leg", "name": "Left Leg", "interval": 1000, "bufferSize": 16,
    "parameters": [[1, 8240, 1], [1, 24676, 0]]
  })");
  auto m = parseMonitoringRequest(body);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->topic, "left-leg");
  ASSERT_TRUE(m->name.has_value());
  EXPECT_EQ(*m->name, "Left Leg");
  EXPECT_EQ(m->interval, std::chrono::milliseconds{1000});
  EXPECT_EQ(m->bufferSize, 16u);
  ASSERT_EQ(m->parameters.size(), 2u);
  EXPECT_EQ(m->parameters[0].devicePosition, 1);
  EXPECT_EQ(m->parameters[0].index, 8240);
  EXPECT_EQ(m->parameters[0].subindex, 1);
  EXPECT_EQ(m->parameters[1].index, 24676);
}

TEST(ParseMonitoringRequest, NameIsOptional) {
  const auto body = nlohmann::json::parse(
      R"({"topic":"t","interval":10,"bufferSize":16,"parameters":[[1,8240,1]]})");
  auto m = parseMonitoringRequest(body);
  ASSERT_TRUE(m.has_value());
  EXPECT_FALSE(m->name.has_value());
}

TEST(ParseMonitoringRequest, RejectsBadShapes) {
  auto bad = [](const char* json) {
    return parseMonitoringRequest(nlohmann::json::parse(json)).has_value();
  };
  EXPECT_FALSE(bad("[]"));                                                         // not an object
  EXPECT_FALSE(bad(R"({"interval":10,"bufferSize":16,"parameters":[[1,1,1]]})"));  // no topic
  EXPECT_FALSE(
      bad(R"({"topic":5,"interval":10,"bufferSize":16,"parameters":[]})"));      // topic not str
  EXPECT_FALSE(bad(R"({"topic":"t","bufferSize":16,"parameters":[[1,1,1]]})"));  // no interval
  EXPECT_FALSE(bad(R"({"topic":"t","interval":10,"parameters":[[1,1,1]]})"));    // no bufferSize
  EXPECT_FALSE(bad(R"({"topic":"t","interval":10,"bufferSize":16})"));           // no parameters
  EXPECT_FALSE(
      bad(R"({"topic":"t","interval":10,"bufferSize":16,"parameters":[[1,1]]})"));  // arity
  EXPECT_FALSE(
      bad(R"({"topic":"t","interval":10,"bufferSize":16,"parameters":[[1,70000,1]]})"));  // out of
                                                                                          // range
}

}  // namespace
