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

// The regression this exists to prevent, and it was a real one: clients percent-encode query values
// (the generated TypeScript client puts every one through encodeURIComponent), so a comma-separated
// list arrives as `1%2C2`. Read undecoded, that parses as no number at all, and `GET
// /api/devices/state` answered 400 "'positions' must be a comma-separated list of numbers" for
// every request the Console made — which reads to a user as being unable to change device states.
TEST(RequestTest, DecodesPercentEncodedQueryValues) {
  const auto req = request("/", {}, "positions=1%2C2&file=a%20b.bin&plus=a+b&pct=100%25");
  EXPECT_EQ(req.query("positions"), "1,2");
  EXPECT_EQ(req.query("file"), "a b.bin") << "%20 is a space";
  EXPECT_EQ(req.query("plus"), "a b") << "'+' is a space in a query";
  EXPECT_EQ(req.query("pct"), "100%");
}

// uWS's semantics, adopted rather than reinvented: a key is only seen when it carries '=', and a
// present-but-empty value reads the same as an absent one. Pinned because an earlier version of
// this class distinguished the two — a distinction of its own invention, which no route wanted and
// which no client could produce, since a bare `?flag` was never matched here in the first place.
TEST(RequestTest, TreatsAnEmptyValueAsAbsent) {
  const auto req = request("/", {}, "readValues=&flag&other=1");
  EXPECT_FALSE(req.query("readValues").has_value()) << "empty value reads as absent";
  EXPECT_FALSE(req.query("flag").has_value()) << "a key with no '=' is not matched";
  EXPECT_FALSE(req.query("readValues2").has_value()) << "a prefix must not match";
  EXPECT_EQ(req.query("other"), "1") << "the rest of the query still parses";
}

// Each lookup decodes into its own copy of the query, so one cannot consume or corrupt another's
// value. uWS's decoder mutates the buffer it is handed, which is exactly why that copy exists.
TEST(RequestTest, RepeatedLookupsAreIndependent) {
  const auto req = request("/", {}, "a=1%2C2&b=x%20y&c=3");
  for (int pass = 0; pass < 3; ++pass) {
    EXPECT_EQ(req.query("a"), "1,2") << "pass " << pass;
    EXPECT_EQ(req.query("c"), "3") << "pass " << pass;
    EXPECT_EQ(req.query("b"), "x y") << "pass " << pass;
  }
}

// The mapping every named parameter read depends on. uWS hands parameters over positionally, so a
// pattern whose names come out in the wrong order — or one short — serves happily and answers with
// a neighbouring segment's value: slavePosition reading an SDO index rather than failing.
TEST(ParameterNames, ReadsNamesInThePatternsOwnOrder) {
  EXPECT_EQ(parameterNames("/api/devices/:slavePosition/sdo/:index/:subindex"),
            (std::vector<std::string>{"slavePosition", "index", "subindex"}));
}

// A trailing parameter has no '/' to end it, which is the boundary case the scan has to get right.
TEST(ParameterNames, EndsTheLastNameAtTheEndOfThePattern) {
  EXPECT_EQ(parameterNames("/api/devices/:slavePosition/procedures/:name"),
            (std::vector<std::string>{"slavePosition", "name"}));
  EXPECT_EQ(parameterNames("/api/monitorings/:topic"), (std::vector<std::string>{"topic"}));
}

// A wildcard route carries no named parameter: /api/user-cache/* reads its path off the URL.
TEST(ParameterNames, FindsNoneInAPatternWithoutParameters) {
  EXPECT_TRUE(parameterNames("/api/user-cache/*").empty());
  EXPECT_TRUE(parameterNames("/api/version").empty());
  EXPECT_TRUE(parameterNames("").empty());
}

TEST(PercentDecode, DecodesEscapes) {
  EXPECT_EQ(percentDecode("v5.6.6%20(rev%202).xml"), "v5.6.6 (rev 2).xml");
  EXPECT_EQ(percentDecode("a%2Fb"), "a/b") << "uppercase hex";
  EXPECT_EQ(percentDecode("a%2fb"), "a/b") << "lowercase hex";
  EXPECT_EQ(percentDecode("nothing-encoded.bin"), "nothing-encoded.bin");
  EXPECT_EQ(percentDecode(""), "");
}

// The reason path and query decoding are separate functions. A query decoder also maps '+' to a
// space, which is right for application/x-www-form-urlencoded and wrong in a path — sharing one
// would look up a file called "a+b.zip" as "a b.zip".
TEST(PercentDecode, LeavesPlusAlone) {
  EXPECT_EQ(percentDecode("a+b.zip"), "a+b.zip");
  EXPECT_EQ(percentDecode("a%2Bb.zip"), "a+b.zip") << "an escaped plus still decodes";
}

// Left verbatim rather than dropped, so a name containing one reaches whatever resolves it intact
// and fails cleanly, instead of silently becoming a different name that might exist.
TEST(PercentDecode, KeepsAnInvalidEscapeVerbatim) {
  EXPECT_EQ(percentDecode("a%4Zb"), "a%4Zb") << "second digit is not hex";
  EXPECT_EQ(percentDecode("a%ZZb"), "a%ZZb") << "neither digit is hex";
  EXPECT_EQ(percentDecode("trailing%"), "trailing%");
  EXPECT_EQ(percentDecode("half%4"), "half%4") << "truncated at the end of input";
  EXPECT_EQ(percentDecode("100%%20done"), "100% done") << "a bare % before a valid escape";
}

// Decoding an encoded traversal is correct and safe: UserCache::resolve validates the decoded path,
// so "%2e%2e" is rejected exactly like a literal "..". Decoding must not hide the spelling from it.
TEST(PercentDecode, DecodesAnEncodedTraversalRatherThanHidingIt) {
  EXPECT_EQ(percentDecode("%2e%2e/etc/passwd"), "../etc/passwd");
  EXPECT_EQ(percentDecode("a%2F%2E%2E%2Fb"), "a/../b");
}

// Multi-byte characters are decoded byte by byte, which is all a path needs — the bytes are handed
// to the filesystem as they are.
TEST(PercentDecode, PassesUtf8BytesThrough) {
  EXPECT_EQ(percentDecode("caf%C3%A9.bin"), "caf\xC3\xA9.bin");
}

// The length is authoritative, never a terminator. Were the result read as a C string, "nul%00byte"
// would truncate to "nul" — a different name, which might well exist.
TEST(PercentDecode, PreservesADecodedNulAndTheLengthAroundIt) {
  const std::string decoded = percentDecode("nul%00byte");
  ASSERT_EQ(decoded.size(), 8u);
  EXPECT_EQ(decoded[3], '\0');
  EXPECT_EQ(decoded, std::string("nul\0byte", 8));
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
