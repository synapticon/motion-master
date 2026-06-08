#include "config.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

using nlohmann::json;

TEST(ConfigTest, EmptyObjectYieldsDefaults) {
  auto r = parseConfig(json::object());
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->server.httpPort, 61447);
  EXPECT_EQ(r->server.wsPort, 62281);
  EXPECT_EQ(r->server.corsOrigin, "https://motion-master.synapticon.com");
  EXPECT_EQ(r->logLevel, "info");
  EXPECT_TRUE(r->tls.autoUpdate);
  EXPECT_TRUE(r->tls.certPath.empty());
  EXPECT_TRUE(r->tls.keyPath.empty());
  EXPECT_TRUE(r->fieldbus.driver.empty());
  EXPECT_TRUE(r->fieldbus.adapter.empty());
}

TEST(ConfigTest, PartialOverrideKeepsOtherDefaults) {
  auto r = parseConfig(json::parse(R"({"server": {"httpPort": 8080}})"));
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->server.httpPort, 8080);
  EXPECT_EQ(r->server.wsPort, 62281);  // sibling key untouched
  EXPECT_EQ(r->logLevel, "info");      // sibling section untouched
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
  for (const char* d : {"soem", "spoe", "igh"}) {
    json doc = {{"fieldbus", {{"driver", d}}}};
    EXPECT_TRUE(parseConfig(doc).has_value()) << d;
  }
}

TEST(ConfigTest, EmptyDriverMeansNoAutoInit) {
  auto r = parseConfig(json::parse(R"({"fieldbus": {"driver": ""}})"));
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_TRUE(r->fieldbus.driver.empty());
}

TEST(ConfigTest, InvalidLogLevelRejected) {
  EXPECT_FALSE(parseConfig(json::parse(R"({"logLevel": "verbose"})")).has_value());
}

TEST(ConfigTest, InvalidDriverRejected) {
  EXPECT_FALSE(parseConfig(json::parse(R"({"fieldbus": {"driver": "ethercat"}})")).has_value());
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
