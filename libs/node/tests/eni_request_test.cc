#include "node/eni_request.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>

#include "etg/eni.h"
#include "etg/tests/eni_fixtures.h"

namespace mm::node {
namespace {

using mm::etg::testing::referenceNetwork;

std::string referenceDocument() {
  const auto written = mm::etg::writeEni(referenceNetwork());
  EXPECT_TRUE(written.has_value()) << written.error();
  return written.value_or("");
}

// The nine-device document ETG ships as a sample: the only ENI here nobody on this project wrote,
// and so the only test that the annotation reads a real file rather than our own output. ETG's
// terms forbid redistributing it, so the test skips when the gitignored directory is empty.
std::string loadComplexSample() {
  std::ifstream in(std::filesystem::path(MM_ENI_SAMPLES_DIR) / "complex.xml", std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

TEST(EniRequestTest, AnnotatesADocumentThisProjectDidNotWrite) {
  const std::string xml = loadComplexSample();
  if (xml.empty()) {
    GTEST_SKIP() << "complex.xml is not present under " << MM_ENI_SAMPLES_DIR;
  }
  const auto response = buildEniResponse(xml);
  ASSERT_TRUE(response.has_value()) << response.error();
  EXPECT_EQ((*response)["summary"]["devices"], 9);
  EXPECT_EQ((*response)["summary"]["datagrams"], 155);
  EXPECT_EQ((*response)["summary"]["coeTransfers"], 6);
  EXPECT_TRUE((*response)["warnings"].empty());

  // Somebody else's tool wrote these commands, and the register names still come out.
  std::size_t annotated = 0;
  std::size_t states = 0;
  for (const nlohmann::json& slave : (*response)["network"]["slaves"]) {
    for (const nlohmann::json& command : slave["initCmds"]) {
      if (!command.contains("register")) {
        continue;
      }
      ++annotated;
      if (command["register"].contains("requestsState") ||
          command["register"].contains("waitsForState")) {
        ++states;
      }
    }
  }
  EXPECT_GT(annotated, 0u);
  EXPECT_GT(states, 0u);
}

TEST(EniRequestTest, RejectsADocumentThatWillNotRead) {
  const auto response = buildEniResponse("not an ENI");
  EXPECT_FALSE(response.has_value());
}

TEST(EniRequestTest, CountsWhatTheDocumentHolds) {
  const auto response = buildEniResponse(referenceDocument());
  ASSERT_TRUE(response.has_value()) << response.error();
  EXPECT_EQ((*response)["summary"]["devices"], 2);
  EXPECT_GT((*response)["summary"]["datagrams"], 0);
  EXPECT_EQ((*response)["summary"]["coeTransfers"], 2);
  EXPECT_TRUE((*response)["warnings"].empty());
}

TEST(EniRequestTest, NamesTheRegisterAnAddressSelects) {
  const auto response = buildEniResponse(referenceDocument());
  ASSERT_TRUE(response.has_value()) << response.error();
  const nlohmann::json& commands = (*response)["network"]["slaves"][0]["initCmds"];

  // The whole point: an address becomes a name, and a repeating block keeps its instance, so a
  // reader can tell sync manager 2 from sync manager 0.
  bool sawSyncManager2 = false;
  for (const nlohmann::json& command : commands) {
    if (command.contains("register") && command["register"]["name"] == "sm2") {
      sawSyncManager2 = true;
      EXPECT_EQ(command["register"]["instance"], 2);
    }
  }
  EXPECT_TRUE(sawSyncManager2);
}

TEST(EniRequestTest, DecodesASyncManagerBlockIntoItsFields) {
  const auto response = buildEniResponse(referenceDocument());
  ASSERT_TRUE(response.has_value()) << response.error();
  for (const nlohmann::json& command : (*response)["network"]["slaves"][0]["initCmds"]) {
    if (command.contains("register") && command["register"]["name"] == "sm2") {
      const nlohmann::json& decoded = command["register"]["decoded"];
      EXPECT_EQ(decoded["physicalStart"], 0x1800);
      EXPECT_EQ(decoded["length"], 0x23);
      EXPECT_EQ(decoded["controlByte"], 0x64);
      EXPECT_EQ(decoded["enabled"], true);
      return;
    }
  }
  FAIL() << "no sync manager 2 write in the reference document";
}

TEST(EniRequestTest, DecodesAnFmmuBlockIntoTheServiceItServes) {
  const auto response = buildEniResponse(referenceDocument());
  ASSERT_TRUE(response.has_value()) << response.error();
  for (const nlohmann::json& command : (*response)["network"]["slaves"][0]["initCmds"]) {
    if (command.contains("register") && command["register"]["name"] == "fmmu0") {
      const nlohmann::json& decoded = command["register"]["decoded"];
      EXPECT_EQ(decoded["physicalStart"], 0x1800);
      EXPECT_EQ(decoded["writes"], true);  // Type 2: the outputs.
      EXPECT_EQ(decoded["reads"], false);
      EXPECT_EQ(decoded["active"], true);
      return;
    }
  }
  FAIL() << "no FMMU 0 write in the reference document";
}

TEST(EniRequestTest, SaysWhichStateACommandRequestsOrWaitsFor) {
  const auto response = buildEniResponse(referenceDocument());
  ASSERT_TRUE(response.has_value()) << response.error();
  bool requests = false;
  bool waits = false;
  for (const nlohmann::json& command : (*response)["network"]["slaves"][0]["initCmds"]) {
    if (!command.contains("register")) {
      continue;
    }
    // A write to AL Control is a request; a read of AL Status with a Validate is the wait, and the
    // state it waits for is in the validate data rather than in a payload.
    if (command["register"].contains("requestsState") &&
        command["register"]["requestsState"] == "SAFE-OP") {
      requests = true;
    }
    if (command["register"].contains("waitsForState") &&
        command["register"]["waitsForState"] == "SAFE-OP") {
      waits = true;
    }
  }
  EXPECT_TRUE(requests);
  EXPECT_TRUE(waits);
}

TEST(EniRequestTest, SpellsOutEveryEnumerationBesideItsValue) {
  const auto response = buildEniResponse(referenceDocument());
  ASSERT_TRUE(response.has_value()) << response.error();
  const nlohmann::json& first = (*response)["network"]["slaves"][0]["initCmds"][0];
  // A person reads the JSON, so a datagram type carries its mnemonic and a payload its hex.
  EXPECT_EQ(first["cmdName"], "FPWR");
  EXPECT_EQ(first["cmd"], 5);
  EXPECT_TRUE(first["data"].contains("hex"));
  EXPECT_TRUE(first["data"].contains("bytes"));
  EXPECT_EQ(first["transitions"][0], "IP");
}

}  // namespace
}  // namespace mm::node
