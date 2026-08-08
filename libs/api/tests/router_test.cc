#include "api/router.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace mm::api {
namespace {

// A request as the framework snapshots one, built directly — which is the point of the value-based
// design: a handler and everything it reads can be exercised with no server, no socket and no loop.
Request request(std::string url = "/api/devices/1/sdo/0x6040/0",
                std::vector<std::pair<std::string, std::string>> parameters = {},
                std::string queryString = {},
                std::vector<std::pair<std::string, std::string>> headers = {},
                std::string body = {}) {
  return Request(std::move(url), std::move(parameters), std::move(queryString), std::move(headers),
                 std::move(body));
}

TEST(RequestTest, ReadsPathParametersByName) {
  const auto req = request("/api/devices/2/sdo/0x6041/1",
                           {{"slavePosition", "2"}, {"index", "0x6041"}, {"subindex", "1"}});
  EXPECT_EQ(req.parameter("slavePosition"), "2");
  EXPECT_EQ(req.parameter("index"), "0x6041");
  EXPECT_EQ(req.parameter("subindex"), "1");
  EXPECT_EQ(req.parameter("nonexistent"), "");
}

// Hex matters: a CoE index is written 0x6041 in its documentation, the Console and the
// specification, so a route that only took decimal would make a user convert by hand.
TEST(RequestTest, ParsesParametersAsNumbersInDecimalAndHex) {
  const auto req = request("/", {{"pos", "2"}, {"index", "0x6041"}, {"upper", "0X6041"}});
  EXPECT_EQ(req.parameterAs<uint16_t>("pos"), 2);
  EXPECT_EQ(req.parameterAs<uint16_t>("index"), 0x6041);
  EXPECT_EQ(req.parameterAs<uint16_t>("upper"), 0x6041);
}

TEST(RequestTest, RejectsParametersThatAreNotWholeNumbers) {
  const auto req = request("/", {{"a", "12abc"}, {"b", ""}, {"c", "-1"}, {"d", "1.5"}});
  EXPECT_FALSE(req.parameterAs<uint16_t>("a").has_value()) << "trailing characters";
  EXPECT_FALSE(req.parameterAs<uint16_t>("b").has_value()) << "empty";
  EXPECT_FALSE(req.parameterAs<uint16_t>("c").has_value()) << "negative into unsigned";
  EXPECT_FALSE(req.parameterAs<uint16_t>("d").has_value()) << "not an integer";
  EXPECT_FALSE(req.parameterAs<uint16_t>("missing").has_value());
}

TEST(RequestTest, ReadsQueryValues) {
  const auto req = request("/", {}, "positions=1,2&readValues=true&depth=8");
  EXPECT_EQ(req.query("positions"), "1,2");
  EXPECT_EQ(req.query("readValues"), "true");
  EXPECT_EQ(req.queryAs<int>("depth"), 8);
  EXPECT_FALSE(req.query("absent").has_value());
}

// Absent and present-but-empty are different requests for some parameters, so the distinction has
// to survive: `?readValues` is a flag, no `readValues` at all is not.
TEST(RequestTest, DistinguishesAnEmptyValueFromAnAbsentKey) {
  const auto req = request("/", {}, "readValues=&other=1");
  auto present = req.query("readValues");
  ASSERT_TRUE(present.has_value());
  EXPECT_EQ(*present, "");
  EXPECT_FALSE(req.query("readValues2").has_value()) << "a prefix must not match";
}

TEST(RequestTest, ReadsHeadersAndNegotiatesContent) {
  const auto req = request("/", {}, "", {{"accept", "application/octet-stream"}});
  EXPECT_EQ(req.header("accept"), "application/octet-stream");
  EXPECT_TRUE(req.accepts("application/octet-stream"));
  EXPECT_FALSE(req.accepts("text/csv"));
  EXPECT_EQ(req.header("absent"), "");
}

TEST(RequestTest, CarriesTheWholeBody) {
  const auto req = request("/", {}, "", {}, R"({"state": 8})");
  EXPECT_EQ(req.body(), R"({"state": 8})");
}

// ── Responses ──────────────────────────────────────────────────────────────────────────────────

TEST(ResponseTest, JsonCarriesTheSerialisedBody) {
  const auto response = json(nlohmann::json{{"ok", true}});
  EXPECT_EQ(response.status, "200 OK");
  EXPECT_EQ(response.contentType, "application/json");
  EXPECT_EQ(response.body, R"({"ok":true})");
}

// A device can return a VISIBLE_STRING that is not valid UTF-8. Throwing here would take the server
// down, so the serialiser replaces rather than throws.
TEST(ResponseTest, JsonSurvivesInvalidUtf8) {
  const auto response = json(nlohmann::json{{"name", std::string("bad\xff"
                                                                 "byte")}});
  EXPECT_FALSE(response.body.empty());
  EXPECT_NE(response.body.find("bad"), std::string::npos);
}

TEST(ResponseTest, ErrorHelpersCarryStatusAndMessage) {
  EXPECT_EQ(badRequest("nope").status, "400 Bad Request");
  EXPECT_EQ(badRequest("nope").body, R"({"error":"nope"})");
  EXPECT_EQ(notFound("gone").status, "404 Not Found");
  EXPECT_EQ(error("409 Conflict", "busy").status, "409 Conflict");
  EXPECT_TRUE(statusOnly("202 Accepted").body.empty());
}

TEST(ResponseTest, WireTimeIsExposedForCrossOriginReads) {
  const auto response = withWireTime(json(nlohmann::json::object()), std::chrono::microseconds(42));
  bool exposed = false;
  bool value = false;
  for (const auto& [name, header] : response.headers) {
    exposed = exposed || (name == "Access-Control-Expose-Headers" && header == "X-Wire-Us");
    value = value || (name == "X-Wire-Us" && header == "42");
  }
  EXPECT_TRUE(exposed) << "a browser cannot read the header cross-origin without this";
  EXPECT_TRUE(value);
}

// The shape nearly every device endpoint uses. Both outcomes carry the timing header, because a
// failed device operation still consumed wire time.
TEST(TimedTest, ReportsSuccessAsJsonWithTiming) {
  const auto response = timed([] { return std::expected<int, std::string>(7); });
  EXPECT_EQ(response.status, "200 OK");
  EXPECT_EQ(response.body, "7");
  EXPECT_EQ(response.headers.size(), 2u);
}

TEST(TimedTest, ReportsFailureWithTheGivenStatusAndTiming) {
  const auto response =
      timed([] { return std::expected<int, std::string>(std::unexpected("no such device")); },
            "404 Not Found");
  EXPECT_EQ(response.status, "404 Not Found");
  EXPECT_EQ(response.body, R"({"error":"no such device"})");
  EXPECT_EQ(response.headers.size(), 2u) << "a failure carries the timing header too";
}

}  // namespace
}  // namespace mm::api
