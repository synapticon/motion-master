#include "monitoring_api.h"

#include <gtest/gtest.h>

namespace {

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

}  // namespace
