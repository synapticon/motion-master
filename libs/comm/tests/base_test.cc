#include "comm/base.h"

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace mm::comm {
namespace {

// ---------------------------------------------------------------------------
// isMacAddress
// ---------------------------------------------------------------------------

TEST(IsMacAddressTest, AcceptsColonSeparated) {
  EXPECT_TRUE(isMacAddress("AA:BB:CC:DD:EE:FF"));
  EXPECT_TRUE(isMacAddress("00:1A:2B:3C:4D:5E"));
}

TEST(IsMacAddressTest, AcceptsDashSeparated) {
  EXPECT_TRUE(isMacAddress("AA-BB-CC-DD-EE-FF"));
  EXPECT_TRUE(isMacAddress("00-1a-2b-3c-4d-5e"));
}

// parseMac

TEST(ParseMacTest, DecodesBothSeparatorsAndEitherCase) {
  const std::array<uint8_t, 6> expected = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  EXPECT_EQ(parseMac("AA:BB:CC:DD:EE:FF"), expected);
  EXPECT_EQ(parseMac("aa-bb-cc-dd-ee-ff"), expected);
}

TEST(ParseMacTest, KeepsTheBytesInTheOrderTheyAreWritten) {
  const std::array<uint8_t, 6> expected = {0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
  EXPECT_EQ(parseMac("00:1A:2B:3C:4D:5E"), expected);
}

TEST(ParseMacTest, RejectsWhatIsNotAMacAddress) {
  EXPECT_FALSE(parseMac("").has_value());
  EXPECT_FALSE(parseMac("AA:BB:CC:DD:EE").has_value());
  EXPECT_FALSE(parseMac("AA:BB:CC:DD:EE:GG").has_value());
  EXPECT_FALSE(parseMac("AA:BB-CC:DD:EE:FF").has_value());  // Mixed separators.
}

TEST(IsMacAddressTest, AcceptsLowercaseHex) { EXPECT_TRUE(isMacAddress("aa:bb:cc:dd:ee:ff")); }

TEST(IsMacAddressTest, RejectsWrongLength) {
  EXPECT_FALSE(isMacAddress(""));
  EXPECT_FALSE(isMacAddress("AA:BB:CC:DD:EE"));        // Too short (14 chars).
  EXPECT_FALSE(isMacAddress("AA:BB:CC:DD:EE:FF:00"));  // Too long (20 chars).
  EXPECT_FALSE(isMacAddress("AA:BB:CC:DD:EE:F"));      // 16 chars.
  EXPECT_FALSE(isMacAddress("AA:BB:CC:DD:EE:FFF"));    // 18 chars.
}

TEST(IsMacAddressTest, RejectsMixedSeparators) {
  EXPECT_FALSE(isMacAddress("AA:BB-CC:DD:EE:FF"));
  EXPECT_FALSE(isMacAddress("AA-BB:CC-DD-EE-FF"));
}

TEST(IsMacAddressTest, RejectsWrongSeparatorCharacter) {
  EXPECT_FALSE(isMacAddress("AA.BB.CC.DD.EE.FF"));
  EXPECT_FALSE(isMacAddress("AA BB CC DD EE FF"));
}

TEST(IsMacAddressTest, RejectsNonHexDigits) {
  EXPECT_FALSE(isMacAddress("GG:BB:CC:DD:EE:FF"));  // 'G' is not hex.
  EXPECT_FALSE(isMacAddress("AA:BB:CC:DD:EE:ZZ"));
}

TEST(IsMacAddressTest, RejectsSeparatorInHexPosition) {
  // A separator where a hex digit is expected (17 chars but malformed).
  EXPECT_FALSE(isMacAddress(":A:BB:CC:DD:EE:FFF"));
}

// ---------------------------------------------------------------------------
// normalizeMac
// ---------------------------------------------------------------------------

TEST(NormalizeMacTest, UppercasesColonInput) {
  EXPECT_EQ(normalizeMac("aa:bb:cc:dd:ee:ff", ':'), "AA:BB:CC:DD:EE:FF");
}

TEST(NormalizeMacTest, PreservesAlreadyUppercase) {
  EXPECT_EQ(normalizeMac("AA:BB:CC:DD:EE:FF", ':'), "AA:BB:CC:DD:EE:FF");
}

TEST(NormalizeMacTest, ConvertsColonToDashSeparator) {
  EXPECT_EQ(normalizeMac("aa:bb:cc:dd:ee:ff", '-'), "AA-BB-CC-DD-EE-FF");
}

TEST(NormalizeMacTest, ConvertsDashToColonSeparator) {
  EXPECT_EQ(normalizeMac("aa-bb-cc-dd-ee-ff", ':'), "AA:BB:CC:DD:EE:FF");
}

TEST(NormalizeMacTest, MixedCaseInput) {
  EXPECT_EQ(normalizeMac("0a:1B:2c:3D:4e:5F", ':'), "0A:1B:2C:3D:4E:5F");
}

TEST(NormalizeMacTest, OutputIsSeventeenChars) {
  EXPECT_EQ(normalizeMac("00:1a:2b:3c:4d:5e", ':').size(), 17u);
}

// ---------------------------------------------------------------------------
// resolveNetworkAdapter
//
// Adapter enumeration depends on the live OS, so these tests exercise only the
// deterministic contract: inputs that cannot match any real adapter must yield
// a specific error string. The chosen MAC/name are extremely unlikely to exist
// on any test host.
// ---------------------------------------------------------------------------

TEST(ResolveNetworkAdapterTest, UnknownMacReportsNormalizedMac) {
  auto result = resolveNetworkAdapter("DE:AD:BE:EF:00:01");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), "no network adapter found with MAC DE:AD:BE:EF:00:01");
}

TEST(ResolveNetworkAdapterTest, LowercaseMacIsNormalizedInError) {
  // A dash-separated lowercase MAC is normalized to colon-separated uppercase
  // before the lookup, and the error echoes that normalized form.
  auto result = resolveNetworkAdapter("de-ad-be-ef-00-01");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), "no network adapter found with MAC DE:AD:BE:EF:00:01");
}

TEST(ResolveNetworkAdapterTest, UnknownInterfaceNameReportsInput) {
  const std::string bogus = "definitely_not_a_real_adapter_xyz";
  auto result = resolveNetworkAdapter(bogus);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), "network adapter '" + bogus + "' not found");
}

// ---------------------------------------------------------------------------
// enumerateNetworkAdapters
//
// The contents depend on the host, but every entry must be fully populated: a
// well-formed colon- and dash-separated MAC and a non-empty interface name,
// with the two MAC forms describing the same address. On a host with no
// adapters (or without privileges) the vector is empty, which is also valid.
// ---------------------------------------------------------------------------

TEST(EnumerateNetworkAdaptersTest, EntriesAreWellFormed) {
  auto adapters = enumerateNetworkAdapters();
  for (const auto& adapter : adapters) {
    EXPECT_TRUE(isMacAddress(adapter.macLinux)) << "macLinux is malformed: " << adapter.macLinux;
    EXPECT_TRUE(isMacAddress(adapter.macWindows))
        << "macWindows is malformed: " << adapter.macWindows;
    EXPECT_EQ(adapter.macWindows, normalizeMac(adapter.macLinux, '-'))
        << "MAC forms disagree for " << adapter.macLinux;
    EXPECT_FALSE(adapter.name.empty()) << "interface name is empty for MAC " << adapter.macLinux;
  }
}

}  // namespace
}  // namespace mm::comm
