#include "etg/fsoe_crc.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace mm::etg {
namespace {

/// The FSoE Safety polynomial, 0x139B7, in the usual 16-bit form with x^16 implicit.
constexpr uint16_t kSafetyPoly = 0x39B7;

/// Generates entry @c i of an MSB-first CRC-16 table for the Safety polynomial.
uint16_t generateTable1Entry(int i) {
  auto crc = static_cast<uint16_t>(i << 8);
  for (int bit = 0; bit < 8; ++bit) {
    const bool msb = (crc & 0x8000) != 0;
    crc = static_cast<uint16_t>(crc << 1);
    if (msb) {
      crc = static_cast<uint16_t>(crc ^ kSafetyPoly);
    }
  }
  return crc;
}

TEST(FsoeCrcTest, Table1IsTheSafetyPolynomialTable) {
  // Table 1 is an ordinary CRC-16 table, so it can be re-derived. A transcription slip in any of
  // its 256 entries would break interoperability with every conforming device, and nothing else
  // in the system would report it.
  const auto table = fsoeCrcTable1();
  ASSERT_EQ(table.size(), 256u);
  for (int i = 0; i < 256; ++i) {
    EXPECT_EQ(table[static_cast<size_t>(i)], generateTable1Entry(i)) << "index " << i;
  }
}

TEST(FsoeCrcTest, BothTablesAreLinearOverGf2) {
  // Any CRC table satisfies tab[a ^ b] == tab[a] ^ tab[b]. Table 2 cannot be re-derived from the
  // polynomial the way table 1 can, so linearity is what pins it: one corrupted entry breaks the
  // identity for many pairs.
  const auto table1 = fsoeCrcTable1();
  const auto table2 = fsoeCrcTable2();
  ASSERT_EQ(table2.size(), 256u);

  for (size_t a = 0; a < 256; ++a) {
    for (size_t b = 0; b < 256; ++b) {
      ASSERT_EQ(table1[a ^ b], table1[a] ^ table1[b]) << "table 1 at " << a << ", " << b;
      ASSERT_EQ(table2[a ^ b], table2[a] ^ table2[b]) << "table 2 at " << a << ", " << b;
    }
  }
}

TEST(FsoeCrcTest, ProducesTheKnownAnswersOfTheFirmwareImplementation) {
  // The same three vectors pin the C implementation these tables were shared with, so the two
  // stacks are known to agree on the wire and not only in intent.
  constexpr std::array<uint8_t, 2> two{0x12, 0x34};
  constexpr std::array<uint8_t, 1> one{0x12};
  constexpr std::array<uint8_t, 2> dead{0xDE, 0xAD};

  EXPECT_EQ(fsoeCrc(0, 0, 1, 0x4E, 0, two, 2), 0x4D07);
  EXPECT_EQ(fsoeCrc(0, 0, 1, 0x4E, 0, one, 1), 0xA176);
  EXPECT_EQ(fsoeCrc(0xABCD, 0x0007, 2, 0x36, 1, dead, 2), 0xBC47);
}

TEST(FsoeCrcTest, AOneOctetBlockIgnoresTheSecondOctet) {
  constexpr std::array<uint8_t, 2> a{0x12, 0x00};
  constexpr std::array<uint8_t, 2> b{0x12, 0xFF};

  EXPECT_EQ(fsoeCrc(0, 0, 1, 0x4E, 0, a, 1), fsoeCrc(0, 0, 1, 0x4E, 0, b, 1));
  EXPECT_NE(fsoeCrc(0, 0, 1, 0x4E, 0, a, 2), fsoeCrc(0, 0, 1, 0x4E, 0, b, 2));
}

TEST(FsoeCrcTest, TheBlockIndexChangesTheCrc) {
  // Two blocks that happen to carry the same octets must not produce the same CRC, or a receiver
  // could not tell a swap of two blocks from an intact frame.
  constexpr std::array<uint8_t, 2> data{0xDE, 0xAD};
  EXPECT_NE(fsoeCrc(0, 7, 2, 0x36, 0, data, 2), fsoeCrc(0, 7, 2, 0x36, 1, data, 2));
}

TEST(FsoeCrcTest, ASingleBitFlipInAnyArgumentChangesTheCrc) {
  constexpr uint16_t recvCrc0 = 0x1234;
  constexpr uint16_t connId = 0x5678;
  constexpr uint16_t seqNo = 0x0009;
  constexpr uint8_t command = 0x36;
  constexpr uint16_t index = 1;
  constexpr std::array<uint8_t, 2> data{0xA5, 0x5A};
  const uint16_t base = fsoeCrc(recvCrc0, connId, seqNo, command, index, data, 2);

  for (int bit = 0; bit < 16; ++bit) {
    const auto mask = static_cast<uint16_t>(1u << bit);
    EXPECT_NE(fsoeCrc(recvCrc0 ^ mask, connId, seqNo, command, index, data, 2), base)
        << "recvCrc0 bit " << bit;
    EXPECT_NE(fsoeCrc(recvCrc0, connId ^ mask, seqNo, command, index, data, 2), base)
        << "connId bit " << bit;
    EXPECT_NE(fsoeCrc(recvCrc0, connId, seqNo ^ mask, command, index, data, 2), base)
        << "seqNo bit " << bit;
  }
  for (int bit = 0; bit < 8; ++bit) {
    const auto mask = static_cast<uint8_t>(1u << bit);
    EXPECT_NE(fsoeCrc(recvCrc0, connId, seqNo, command ^ mask, index, data, 2), base)
        << "command bit " << bit;
    const std::array<uint8_t, 2> flip0{static_cast<uint8_t>(data[0] ^ mask), data[1]};
    const std::array<uint8_t, 2> flip1{data[0], static_cast<uint8_t>(data[1] ^ mask)};
    EXPECT_NE(fsoeCrc(recvCrc0, connId, seqNo, command, index, flip0, 2), base)
        << "data[0] bit " << bit;
    EXPECT_NE(fsoeCrc(recvCrc0, connId, seqNo, command, index, flip1, 2), base)
        << "data[1] bit " << bit;
  }
}

TEST(FsoeCrcTest, RefusesABlockLengthTheStandardDoesNotDefine) {
  constexpr std::array<uint8_t, 2> data{0x12, 0x34};
  EXPECT_EQ(fsoeCrc(0, 0, 1, 0x4E, 0, data, 0), 0);
  EXPECT_EQ(fsoeCrc(0, 0, 1, 0x4E, 0, data, 3), 0);
  EXPECT_EQ(fsoeCrc(0, 0, 1, 0x4E, 0, std::span<const uint8_t>{}, 1), 0);
}

}  // namespace
}  // namespace mm::etg
