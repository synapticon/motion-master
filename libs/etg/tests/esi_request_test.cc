#include "etg/esi_request.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace mm::etg {
namespace {

// The response builder is the whole of the POST /api/esi/parse route: the handler accumulates the
// body, reads one optional query parameter and forwards. Testing it here rather than over HTTP
// keeps the contract covered without standing up a server.
std::string somanetXml() {
  static const std::string* xml = [] {
    const auto path = std::filesystem::path(MM_ETG_TEST_DATA_DIR) / "somanet-v5.6.6.xml";
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << path.string();
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return new std::string(buffer.str());
  }();
  return *xml;
}

const nlohmann::json& deviceAt(const nlohmann::json& response, std::size_t ordinal) {
  return response.at("devices").at(ordinal);
}

bool collided(const nlohmann::json& device) {
  if (!device.contains("warnings")) {
    return false;
  }
  return std::any_of(
      device.at("warnings").begin(), device.at("warnings").end(), [](const nlohmann::json& w) {
        return w.get<std::string>().find("each declare the same") != std::string::npos;
      });
}

TEST(EsiRequestTest, ReturnsEveryDeviceWithItsOwnAssembledDictionary) {
  const auto response = buildEsiResponse(somanetXml(), EsiParseRequest{});
  ASSERT_TRUE(response.has_value()) << (response ? std::string{} : response.error());

  EXPECT_EQ(response->at("vendor").at("id"), 0x22D2u);
  ASSERT_EQ(response->at("devices").size(), 4u);
  ASSERT_EQ(response->at("modules").size(), 5u);

  // There is no device selector: every device arrives with its entries already assembled.
  for (const nlohmann::json& device : response->at("devices")) {
    EXPECT_TRUE(device.contains("entries")) << device.at("type");
    EXPECT_GT(device.at("entries").size(), 100u) << device.at("type");
  }

  const nlohmann::json& node = deviceAt(*response, 0);
  EXPECT_EQ(node.at("ordinal"), 0);
  EXPECT_EQ(node.at("type"), "SOMANET Node");
  EXPECT_EQ(node.at("productCode"), 0x00000201u);
  EXPECT_EQ(node.at("revisionNo"), 0x11000003u);
  EXPECT_EQ(node.at("profileNo"), 402);
  // objectCount is the device's OWN dictionary, before any module merge — deliberately smaller
  // than the assembled table, since every CiA402 object lives in a module.
  EXPECT_LT(node.at("objectCount").get<std::size_t>(), node.at("entries").size());

  const nlohmann::json& module = response->at("modules").at(0);
  EXPECT_EQ(module.at("moduleIdent"), 0x04020001u);
  EXPECT_EQ(module.at("objectCount"), 128u);
}

TEST(EsiRequestTest, EntriesCarryTheMetadataOnlyAnEsiHas) {
  const auto response = buildEsiResponse(somanetXml(), EsiParseRequest{});
  ASSERT_TRUE(response.has_value()) << (response ? std::string{} : response.error());

  const nlohmann::json& entries = deviceAt(*response, 0).at("entries");
  const auto controlword = std::find_if(
      entries.begin(), entries.end(),
      [](const nlohmann::json& e) { return e.at("index") == 0x6040 && e.at("subindex") == 0; });
  ASSERT_NE(controlword, entries.end());
  EXPECT_EQ(controlword->at("objectName"), "Controlword");
  EXPECT_EQ(controlword->at("dataTypeName"), "UINT");
  EXPECT_EQ(controlword->at("source").at("kind"), "module");
}

TEST(EsiRequestTest, StoresObjectAnnotationOnceOnSubindexZero) {
  // The reason every device can be returned at once. Repeating an object's description onto each
  // of its subindices made a single device 4.7 MB of JSON, 83% of it the same HTML over and over.
  // Subindex 0 *is* the object, so its annotation lives there and nowhere else.
  const auto response = buildEsiResponse(somanetXml(), EsiParseRequest{});
  ASSERT_TRUE(response.has_value()) << (response ? std::string{} : response.error());
  const nlohmann::json& entries = deviceAt(*response, 0).at("entries");

  std::size_t composites = 0;
  for (const nlohmann::json& entry : entries) {
    if (entry.at("objectCode") == "VAR" || entry.at("subindex") == 0 ||
        !entry.contains("description")) {
      continue;
    }
    ++composites;
    // A non-zero subindex may describe itself — a RECORD member often does — but must never
    // repeat its object's description.
    const auto si0 =
        std::find_if(entries.begin(), entries.end(), [&entry](const nlohmann::json& c) {
          return c.at("index") == entry.at("index") && c.at("subindex") == 0;
        });
    ASSERT_NE(si0, entries.end());
    if (si0->contains("description")) {
      EXPECT_NE(si0->at("description"), entry.at("description"))
          << "object description repeated on " << entry.at("index") << ":" << entry.at("subindex");
    }
  }
  EXPECT_GT(composites, 0u) << "no described composite members — the loop proved nothing";
}

TEST(EsiRequestTest, ARecordMemberKeepsItsOwnDescription) {
  // The counterpart to the rule above: collapsing object annotation onto subindex 0 must not cost
  // a RECORD member the description the ESI wrote for it specifically.
  const auto response = buildEsiResponse(somanetXml(), EsiParseRequest{});
  ASSERT_TRUE(response.has_value()) << (response ? std::string{} : response.error());
  const nlohmann::json& entries = deviceAt(*response, 0).at("entries");

  EXPECT_TRUE(std::any_of(entries.begin(), entries.end(), [](const nlohmann::json& e) {
    return e.at("objectCode") == "RECORD" && e.at("subindex") != 0 && e.contains("description");
  }));
}

TEST(EsiRequestTest, NarrowsTheModuleMergeToAConcreteConfiguration) {
  // The Circulo SMM's second slot offers four mutually exclusive FSoE variants. Merging all of
  // them is the honest default for an offline tool, but a caller who knows the configuration can
  // ask for it and get a table with no collisions.
  const auto merged = buildEsiResponse(somanetXml(), EsiParseRequest{});
  ASSERT_TRUE(merged.has_value()) << (merged ? std::string{} : merged.error());

  EXPECT_TRUE(collided(deviceAt(*merged, 2)));
  EXPECT_FALSE(collided(deviceAt(*merged, 0))) << "a single-slot device cannot collide";

  // Naming idents applies to every device, by intersection with what each one references — so the
  // single-slot devices are untouched while the safe-motion device is pinned to one variant.
  EsiParseRequest narrowed;
  narrowed.moduleIdents = {0x04020001, 0x22D20001};
  const auto single = buildEsiResponse(somanetXml(), narrowed);
  ASSERT_TRUE(single.has_value()) << (single ? std::string{} : single.error());

  EXPECT_FALSE(collided(deviceAt(*single, 2)));
  EXPECT_LT(deviceAt(*single, 2).at("entries").size(), deviceAt(*merged, 2).at("entries").size());
  EXPECT_EQ(deviceAt(*single, 0).at("entries").size(), deviceAt(*merged, 0).at("entries").size());
}

TEST(EsiRequestTest, SeparatesDocumentWarningsFromPerDeviceWarnings) {
  // A document warning is a property of the file; a device warning depends on which modules were
  // merged into that device. Mixing them would make the second look permanent.
  const auto response = buildEsiResponse(somanetXml(), EsiParseRequest{});
  ASSERT_TRUE(response.has_value()) << (response ? std::string{} : response.error());

  EXPECT_FALSE(response->contains("warnings"));  // This file parses cleanly.
  EXPECT_TRUE(deviceAt(*response, 0).contains("warnings"));
  EXPECT_FALSE(deviceAt(*response, 0).at("warnings").empty());
}

TEST(EsiRequestTest, PropagatesAParseFailure) {
  const auto bad = buildEsiResponse("<not-esi/>", EsiParseRequest{});
  ASSERT_FALSE(bad.has_value());
  EXPECT_NE(bad.error().find("EtherCATInfo"), std::string::npos) << bad.error();
}

TEST(EsiRequestTest, ADeviceWithoutADictionaryDoesNotFailTheWholeFile) {
  // One bare device among several must cost only its own entries. Failing the request would make
  // an otherwise readable file unreadable.
  const std::string xml = R"(<?xml version="1.0"?>
<EtherCATInfo>
  <Vendor><Id>#x22D2</Id></Vendor>
  <Descriptions><Devices>
    <Device><Type ProductCode="#x1" RevisionNo="#x1">Bare</Type></Device>
    <Device>
      <Type ProductCode="#x2" RevisionNo="#x1">Full</Type>
      <Profile><ProfileNo>402</ProfileNo><Dictionary>
        <DataTypes><DataType><Name>UDINT</Name><BitSize>32</BitSize></DataType></DataTypes>
        <Objects><Object><Index>#x1000</Index><Name>Device type</Name><Type>UDINT</Type>
          <BitSize>32</BitSize></Object></Objects>
      </Dictionary></Profile>
    </Device>
  </Devices></Descriptions>
</EtherCATInfo>)";

  const auto response = buildEsiResponse(xml, EsiParseRequest{});
  ASSERT_TRUE(response.has_value()) << (response ? std::string{} : response.error());
  ASSERT_EQ(response->at("devices").size(), 2u);

  EXPECT_TRUE(deviceAt(*response, 0).at("entries").empty());
  EXPECT_FALSE(deviceAt(*response, 0).at("warnings").empty());
  EXPECT_EQ(deviceAt(*response, 1).at("entries").size(), 1u);
}

TEST(EsiRequestTest, ParsesAnIdentListInEveryAcceptedNotation) {
  const auto mixed = parseIdentList("#x04020001, 0x22D20001 ,67108865");
  ASSERT_TRUE(mixed.has_value()) << (mixed ? std::string{} : mixed.error());
  EXPECT_EQ(*mixed, (std::vector<uint32_t>{0x04020001u, 0x22D20001u, 0x04000001u}));

  EXPECT_TRUE(parseIdentList("").value().empty());
  EXPECT_TRUE(parseIdentList("   ").value().empty());
  // A trailing separator is sloppy but unambiguous; rejecting it would help nobody.
  EXPECT_EQ(parseIdentList("#x1,").value().size(), 1u);

  const auto bad = parseIdentList("#x1,nonsense");
  ASSERT_FALSE(bad.has_value());
  EXPECT_NE(bad.error().find("nonsense"), std::string::npos) << bad.error();
}

}  // namespace
}  // namespace mm::etg
