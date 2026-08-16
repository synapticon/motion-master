#include "config.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

using nlohmann::json;

TEST(ConfigTest, EmptyObjectYieldsDefaults) {
  auto r = parseConfig(json::object());
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->server.bindAddress, "127.0.0.1");
  EXPECT_EQ(r->server.httpPort, 61447);
  EXPECT_EQ(r->server.wsPort, 62281);
  EXPECT_EQ(r->server.corsOrigin, "https://motion-master.synapticon.com");
  EXPECT_EQ(r->logging.level, "info");
  // The file keeps its own, more verbose level on purpose — the console stays readable while the
  // file holds what a support request needs.
  EXPECT_TRUE(r->logging.file.enabled);
  EXPECT_EQ(r->logging.file.level, "debug");
  EXPECT_TRUE(r->logging.file.directory.empty());
  EXPECT_EQ(r->logging.file.maxSizeMb, 10u);
  EXPECT_EQ(r->logging.file.maxFiles, 5u);
  EXPECT_TRUE(r->tls.autoUpdate);
  EXPECT_TRUE(r->tls.certPath.empty());
  EXPECT_TRUE(r->tls.keyPath.empty());
  EXPECT_TRUE(r->fieldbus.driver.empty());
  EXPECT_TRUE(r->fieldbus.adapter.empty());
  EXPECT_EQ(r->gameLoop.periodUs, 1000u);
}

TEST(ConfigTest, GameLoopPeriodOverride) {
  auto r = parseConfig(json::parse(R"({"gameLoop": {"periodUs": 250}})"));
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->gameLoop.periodUs, 250u);
}

TEST(ConfigTest, ZeroGameLoopPeriodRejected) {
  EXPECT_FALSE(parseConfig(json::parse(R"({"gameLoop": {"periodUs": 0}})")).has_value());
}

TEST(ConfigTest, CpuAffinityDefaultsToUnpinned) {
  auto r = parseConfig(json::object());
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->gameLoop.cpuAffinity, -1);
}

TEST(ConfigTest, CpuAffinityOverride) {
  auto r = parseConfig(json::parse(R"({"gameLoop": {"cpuAffinity": 0}})"));
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->gameLoop.cpuAffinity, 0);
}

TEST(ConfigTest, CpuAffinityBeyondCpuCountRejected) {
  // Caught here rather than failing silently at startup, where sched_setaffinity
  // would just return EINVAL and leave the thread unpinned.
  EXPECT_FALSE(parseConfig(json::parse(R"({"gameLoop": {"cpuAffinity": 1048576}})")).has_value());
}

TEST(ConfigTest, PartialOverrideKeepsOtherDefaults) {
  auto r = parseConfig(json::parse(R"({"server": {"httpPort": 8080}})"));
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->server.httpPort, 8080);
  EXPECT_EQ(r->server.wsPort, 62281);   // sibling key untouched
  EXPECT_EQ(r->logging.level, "info");  // sibling section untouched
}

TEST(ConfigTest, NestedPartialFieldbus) {
  auto r = parseConfig(json::parse(R"({"fieldbus": {"driver": "soem"}})"));
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->fieldbus.driver, "soem");
  EXPECT_TRUE(r->fieldbus.adapter.empty());
}

TEST(ConfigTest, TlsBlockParsed) {
  auto r = parseConfig(json::parse(R"({"tls": {"autoUpdate": false, "certPath": "/x/c.pem"}})"));
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_FALSE(r->tls.autoUpdate);
  EXPECT_EQ(r->tls.certPath, "/x/c.pem");
  EXPECT_TRUE(r->tls.keyPath.empty());
}

TEST(ConfigTest, AllValidDriversAccepted) {
  for (const char* d : {"soem", "spoe"}) {
    json doc = {{"fieldbus", {{"driver", d}}}};
    EXPECT_TRUE(parseConfig(doc).has_value()) << d;
  }
}

TEST(ConfigTest, EmptyDriverMeansNoAutoInit) {
  auto r = parseConfig(json::parse(R"({"fieldbus": {"driver": ""}})"));
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_TRUE(r->fieldbus.driver.empty());
}

TEST(ConfigTest, BindAddressOverride) {
  auto r = parseConfig(json::parse(R"({"server": {"bindAddress": "0.0.0.0"}})"));
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->server.bindAddress, "0.0.0.0");
}

TEST(ConfigTest, EmptyBindAddressRejected) {
  // uWebSockets would read "" as every interface — binding an unauthenticated server to the whole
  // network must be spelled "0.0.0.0", never fallen into.
  EXPECT_FALSE(parseConfig(json::parse(R"({"server": {"bindAddress": ""}})")).has_value());
}

TEST(ConfigTest, InvalidLogLevelRejected) {
  EXPECT_FALSE(parseConfig(json::parse(R"({"logging": {"level": "verbose"}})")).has_value());
  // The file's level is validated in its own right — it is not derived from the console's.
  EXPECT_FALSE(
      parseConfig(json::parse(R"({"logging": {"file": {"level": "verbose"}}})")).has_value());
}

TEST(ConfigTest, InvalidLogRotationRejected) {
  // Zero either way makes rotation meaningless: no size to fill, or nowhere to rotate to.
  EXPECT_FALSE(parseConfig(json::parse(R"({"logging": {"file": {"maxSizeMb": 0}}})")).has_value());
  EXPECT_FALSE(parseConfig(json::parse(R"({"logging": {"file": {"maxFiles": 0}}})")).has_value());
}

TEST(ConfigTest, PartialFileLoggingKeepsSiblingDefaults) {
  // Two levels of nesting, so this pins that _WITH_DEFAULT recurses: naming one key of
  // logging.file must not reset the others, nor logging.level above them.
  auto r = parseConfig(json::parse(R"({"logging": {"file": {"maxFiles": 2}}})"));
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->logging.file.maxFiles, 2u);
  EXPECT_EQ(r->logging.file.maxSizeMb, 10u);
  EXPECT_EQ(r->logging.file.level, "debug");
  EXPECT_TRUE(r->logging.file.enabled);
  EXPECT_EQ(r->logging.level, "info");
}

TEST(ConfigTest, InvalidDriverRejected) {
  // "igh" was dropped from the accepted set; an unknown driver is likewise rejected.
  for (const char* d : {"ethercat", "igh"}) {
    json doc = {{"fieldbus", {{"driver", d}}}};
    EXPECT_FALSE(parseConfig(doc).has_value()) << d;
  }
}

TEST(ConfigTest, WrongScalarTypeRejected) {
  EXPECT_FALSE(parseConfig(json::parse(R"({"server": {"httpPort": "nope"}})")).has_value());
}

TEST(ConfigTest, WrongSectionTypeRejected) {
  EXPECT_FALSE(parseConfig(json::parse(R"({"server": 5})")).has_value());
}

TEST(ConfigTest, NonObjectTopLevelRejected) {
  EXPECT_FALSE(parseConfig(json::array()).has_value());
}

TEST(ConfigTest, UnknownKeysIgnored) {
  auto r = parseConfig(json::parse(R"({"unknownTop": 1, "server": {"bogus": 2, "httpPort": 5}})"));
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->server.httpPort, 5);
}

TEST(ConfigTest, JsoncCommentsAreParsed) {
  const char* text = R"({
    // line comment
    "server": { "httpPort": 1234 } /* block comment */
  })";
  auto doc = json::parse(text, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
  auto r = parseConfig(doc);
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->server.httpPort, 1234);
}
