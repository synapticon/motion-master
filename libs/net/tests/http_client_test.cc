#include "net/http_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

// These tests need no server and no network. They cover the boundary this library owns: a request
// that never reaches a server must come back as an error string that names the URL, rather than as
// a Response with a zero status that a caller could mistake for a reply. Anything beyond that needs
// a real endpoint, and the auto-tuning integration tests are where that happens.
namespace {

constexpr std::chrono::seconds kTimeout{5};

// Nothing listens on port 1, and connecting is refused immediately, so this stays fast and offline.
constexpr const char* kRefusedUrl = "http://127.0.0.1:1/api/health";

TEST(HttpClientTest, GetOnRefusedPortReportsTheUrl) {
  const mm::net::HttpGlobal global;

  const auto result = mm::net::httpGet(kRefusedUrl, kTimeout);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find(kRefusedUrl), std::string::npos);
}

TEST(HttpClientTest, PostOnRefusedPortReportsTheUrl) {
  const mm::net::HttpGlobal global;

  const auto result =
      mm::net::httpPost(kRefusedUrl, R"({"run":"exit"})", "application/json", kTimeout);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find(kRefusedUrl), std::string::npos);
}

TEST(HttpClientTest, MalformedUrlIsAnError) {
  const mm::net::HttpGlobal global;

  const auto result = mm::net::httpGet("this is not a url", kTimeout);
  EXPECT_FALSE(result.has_value());
}

}  // namespace
