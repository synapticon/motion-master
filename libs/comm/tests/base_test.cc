#include "comm/base.h"

#include <gtest/gtest.h>

#include <array>
#include <string>

#include "comm/fieldbus_driver.h"

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

// Sync Manager and FMMU register codecs

TEST(RegisterCodecTest, ASyncManagerSurvivesARoundTripExceptItsType) {
  SyncManagerConfig config{
      .index = 2, .physicalStart = 0x1800, .length = 6, .flags = 0x10064, .type = 3};
  const auto bytes = encodeSyncManager(config);
  const auto decoded = decodeSyncManager(config.index, bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->index, config.index);
  EXPECT_EQ(decoded->physicalStart, config.physicalStart);
  EXPECT_EQ(decoded->length, config.length);
  EXPECT_EQ(decoded->flags, config.flags);
  // What the channel carries is the master's classification from the SII, not a register field.
  EXPECT_EQ(decoded->type, 0);
}

TEST(RegisterCodecTest, ASyncManagerBlockIsEightBytesInRegisterOrder) {
  const SyncManagerConfig config{
      .index = 2, .physicalStart = 0x1800, .length = 6, .flags = 0x10064, .type = 3};
  const auto bytes = encodeSyncManager(config);
  // Start and length little-endian, then control, status, activate and PDI control. The two the
  // master may not set are written as zero.
  const std::array<uint8_t, 8> expected = {0x00, 0x18, 0x06, 0x00, 0x64, 0x00, 0x01, 0x00};
  EXPECT_EQ(bytes, expected);
}

TEST(RegisterCodecTest, AnFmmuSurvivesARoundTripWhole) {
  const FmmuConfig config{.index = 1,
                          .logicalStart = 0x00010006,
                          .length = 6,
                          .logicalStartBit = 3,
                          .logicalEndBit = 7,
                          .physicalStart = 0x1C00,
                          .physicalStartBit = 2,
                          .type = 1,
                          .active = 1};
  const auto decoded = decodeFmmu(config.index, encodeFmmu(config));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->index, config.index);
  EXPECT_EQ(decoded->logicalStart, config.logicalStart);
  EXPECT_EQ(decoded->length, config.length);
  EXPECT_EQ(decoded->logicalStartBit, config.logicalStartBit);
  EXPECT_EQ(decoded->logicalEndBit, config.logicalEndBit);
  EXPECT_EQ(decoded->physicalStart, config.physicalStart);
  EXPECT_EQ(decoded->physicalStartBit, config.physicalStartBit);
  EXPECT_EQ(decoded->type, config.type);
  EXPECT_EQ(decoded->active, config.active);
}

TEST(RegisterCodecTest, RefusesAShortBlock) {
  const std::array<uint8_t, 4> short4{};
  EXPECT_FALSE(decodeSyncManager(0, short4).has_value());
  EXPECT_FALSE(decodeFmmu(0, short4).has_value());
}

TEST(RegisterCodecTest, TakesTheRegisterAddressFromTheIndexAndTheStride) {
  // The addresses an init command targets, so a reader can recognise a block write by its Ado.
  EXPECT_EQ(kSyncManagerRegisterBase + 2 * kSyncManagerRegisterBytes, 0x0810);
  EXPECT_EQ(kFmmuRegisterBase + 1 * kFmmuRegisterBytes, 0x0610);
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
