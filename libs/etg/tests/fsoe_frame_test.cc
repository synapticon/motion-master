#include "etg/fsoe_frame.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace mm::etg {
namespace {

std::vector<uint8_t> buildPdu(uint16_t safeDataLen, FsoeCommand command,
                              std::span<const uint8_t> data, uint16_t connId, uint16_t recvCrc0,
                              uint16_t seqNo, uint16_t* crc0 = nullptr) {
  const FsoeFrameLayout layout(safeDataLen);
  std::vector<uint8_t> pdu(layout.size(), 0);
  fsoeFrameSetCommand(pdu, static_cast<uint8_t>(command));
  fsoeFrameSetConnId(pdu, layout, connId);
  fsoeFrameWriteSafeData(pdu, layout, data);
  const uint16_t built = fsoePduBuildCrcs(pdu, layout, recvCrc0, seqNo);
  if (crc0 != nullptr) {
    *crc0 = built;
  }
  return pdu;
}

TEST(FsoeFrameTest, AFrameGrowsByFourOctetsPerBlockNotByOne) {
  // Every two SafeData octets carry their own CRC, so the frame is 1 + 2n + 2. Sizing a PDO from
  // 1 + n + 2 produces a frame the peer silently drops, which reads as a dead connection with no
  // error anywhere.
  EXPECT_EQ(FsoeFrameLayout(2).size(), 7);
  EXPECT_EQ(FsoeFrameLayout(4).size(), 11);
  EXPECT_EQ(FsoeFrameLayout(6).size(), 15);

  // The pair a Synapticon drive with the safe-sensor option uses: 8 octets out, 12 octets back.
  EXPECT_EQ(FsoeFrameLayout(8).size(), 19);
  EXPECT_EQ(FsoeFrameLayout(12).size(), 27);
}

TEST(FsoeFrameTest, TheOneOctetCaseIsTheSixOctetMinimumFrame) {
  const FsoeFrameLayout layout(1);
  EXPECT_TRUE(layout.valid());
  EXPECT_EQ(layout.size(), kFsoeMinPduSize);
  EXPECT_EQ(layout.blockCount(), 1);
  EXPECT_EQ(layout.blockDataLen(), 1);
  EXPECT_EQ(layout.dataOffset(0), 1);
  EXPECT_EQ(layout.crcOffset(0), 2);
  EXPECT_EQ(layout.connIdOffset(), 4);
}

TEST(FsoeFrameTest, AnIllegalSafeDataLengthYieldsNoLayoutAtAll) {
  // A length the standard does not allow must not produce plausible offsets. Zero size and zero
  // blocks make every accessor refuse, which is what turns a misconfiguration into a dead
  // connection instead of into traffic nobody can validate.
  for (const uint16_t n : {uint16_t{0}, uint16_t{3}, uint16_t{5}, uint16_t{31}}) {
    const FsoeFrameLayout layout(n);
    EXPECT_FALSE(layout.valid()) << "n " << n;
    EXPECT_EQ(layout.size(), 0) << "n " << n;
    EXPECT_EQ(layout.blockCount(), 0) << "n " << n;
  }
}

TEST(FsoeFrameTest, BlocksAndCrcsAlternateAlongTheFrame) {
  const FsoeFrameLayout layout(8);
  EXPECT_EQ(layout.blockCount(), 4);
  EXPECT_EQ(layout.blockDataLen(), 2);
  EXPECT_EQ(layout.dataOffset(0), 1);
  EXPECT_EQ(layout.crcOffset(0), 3);
  EXPECT_EQ(layout.dataOffset(1), 5);
  EXPECT_EQ(layout.crcOffset(1), 7);
  EXPECT_EQ(layout.dataOffset(3), 13);
  EXPECT_EQ(layout.crcOffset(3), 15);
  EXPECT_EQ(layout.connIdOffset(), 17);
}

TEST(FsoeFrameTest, ReadsBackWhatItWrote) {
  const FsoeFrameLayout layout(12);
  constexpr std::array<uint8_t, 12> data{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  const std::vector<uint8_t> pdu = buildPdu(12, FsoeCommand::ProcessData, data, 0x0007, 0xABCD, 3);

  EXPECT_EQ(fsoeFrameCommand(pdu), static_cast<uint8_t>(FsoeCommand::ProcessData));
  EXPECT_EQ(fsoeFrameConnId(pdu, layout), 0x0007);

  std::array<uint8_t, 12> readBack{};
  EXPECT_EQ(fsoeFrameReadSafeData(pdu, layout, readBack), 12);
  EXPECT_EQ(readBack, data);
}

TEST(FsoeFrameTest, ShortSafeDataIsZeroFilledToTheFrameLength) {
  // The handshake sends 2 or 4 meaningful octets in a frame that carries 8 or 12. The rest has to
  // be zero, because the peer includes every octet in its CRC.
  const FsoeFrameLayout layout(8);
  constexpr std::array<uint8_t, 2> data{0xAA, 0xBB};
  const std::vector<uint8_t> pdu = buildPdu(8, FsoeCommand::Session, data, 0, 0, 1);

  std::array<uint8_t, 8> readBack{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_EQ(fsoeFrameReadSafeData(pdu, layout, readBack), 8);
  EXPECT_EQ(readBack, (std::array<uint8_t, 8>{0xAA, 0xBB, 0, 0, 0, 0, 0, 0}));
}

TEST(FsoeFrameTest, ChecksTheFrameItBuilt) {
  const FsoeFrameLayout layout(8);
  constexpr std::array<uint8_t, 8> data{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  uint16_t crc0 = 0;
  const std::vector<uint8_t> pdu =
      buildPdu(8, FsoeCommand::ProcessData, data, 0x0007, 0x1234, 9, &crc0);

  const FsoeCheckResult result = fsoePduCheck(pdu, layout, 0x1234, 9);
  EXPECT_TRUE(result.crcOk);
  EXPECT_EQ(result.crc0, crc0);
  EXPECT_EQ(result.crc0, fsoeFrameReadCrc(pdu, layout, 0));
}

TEST(FsoeFrameTest, AnyCorruptedOctetFailsTheCheck) {
  const FsoeFrameLayout layout(8);
  constexpr std::array<uint8_t, 8> data{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  const std::vector<uint8_t> original =
      buildPdu(8, FsoeCommand::ProcessData, data, 0x0007, 0x1234, 9);

  for (size_t i = 0; i < original.size(); ++i) {
    std::vector<uint8_t> corrupted = original;
    corrupted[i] ^= 0x01;
    EXPECT_FALSE(fsoePduCheck(corrupted, layout, 0x1234, 9).crcOk) << "octet " << i;
  }
}

TEST(FsoeFrameTest, TheChainAndTheSequenceNumberAreBothPartOfTheCheck) {
  // These two inputs are what make a frame valid only in its place in one connection. A recording
  // replayed later carries intact CRCs and still fails, which is the whole point of the chain.
  const FsoeFrameLayout layout(8);
  constexpr std::array<uint8_t, 8> data{1, 2, 3, 4, 5, 6, 7, 8};
  const std::vector<uint8_t> pdu = buildPdu(8, FsoeCommand::ProcessData, data, 0x0007, 0x1234, 9);

  EXPECT_TRUE(fsoePduCheck(pdu, layout, 0x1234, 9).crcOk);
  EXPECT_FALSE(fsoePduCheck(pdu, layout, 0x1235, 9).crcOk);
  EXPECT_FALSE(fsoePduCheck(pdu, layout, 0x1234, 10).crcOk);
}

TEST(FsoeFrameTest, EveryBlockIsCheckedNotOnlyTheFirst) {
  // A frame whose CRC_0 is intact and whose later blocks are not must fail. Stopping after the
  // first block would leave most of the payload unprotected.
  const FsoeFrameLayout layout(8);
  constexpr std::array<uint8_t, 8> data{1, 2, 3, 4, 5, 6, 7, 8};
  std::vector<uint8_t> pdu = buildPdu(8, FsoeCommand::ProcessData, data, 0x0007, 0, 1);

  pdu[layout.crcOffset(3)] ^= 0x80;
  const FsoeCheckResult result = fsoePduCheck(pdu, layout, 0, 1);
  EXPECT_FALSE(result.crcOk);
  // CRC_0 is still reported, because the caller needs it to explain what it saw.
  EXPECT_EQ(result.crc0, fsoeFrameReadCrc(pdu, layout, 0));
}

TEST(FsoeFrameTest, AnEmptyOrShortSpanReadsAsZeroInsteadOfPastTheEnd) {
  const FsoeFrameLayout layout(8);
  const std::vector<uint8_t> tooShort(4, 0xFF);

  EXPECT_EQ(fsoeFrameCommand(std::span<const uint8_t>{}), 0);
  EXPECT_EQ(fsoeFrameConnId(tooShort, layout), 0);
  EXPECT_EQ(fsoeFrameReadCrc(tooShort, layout, 3), 0);

  std::array<uint8_t, 2> block{};
  EXPECT_EQ(fsoeFrameReadBlock(tooShort, layout, 2, block), 0);
  EXPECT_EQ(fsoeFrameReadBlock(tooShort, layout, 0, std::span<uint8_t>{}), 0);
}

TEST(FsoeFrameTest, AnOutOfRangeBlockIndexIsRefused) {
  const FsoeFrameLayout layout(4);
  std::vector<uint8_t> pdu(layout.size(), 0);
  const std::vector<uint8_t> before = pdu;

  constexpr std::array<uint8_t, 2> data{0xAA, 0xBB};
  fsoeFrameWriteBlock(pdu, layout, 2, data, 2);
  fsoeFrameWriteCrc(pdu, layout, 9, 0xFFFF);
  EXPECT_EQ(pdu, before);
}

TEST(FsoeFrameTest, AnIllegalLayoutNeverPassesACheck) {
  const FsoeFrameLayout layout(3);
  const std::vector<uint8_t> pdu(11, 0);
  EXPECT_FALSE(fsoePduCheck(pdu, layout, 0, 1).crcOk);
}

TEST(FsoeFrameTest, NamesEveryCommandAndError) {
  EXPECT_EQ(fsoeCommandName(FsoeCommand::ProcessData), "ProcessData");
  EXPECT_EQ(fsoeCommandName(static_cast<FsoeCommand>(0x99)), "?");
  EXPECT_EQ(fsoeErrorName(FsoeError::WatchdogExpired), "WatchdogExpired");
  EXPECT_EQ(fsoeErrorName(static_cast<FsoeError>(0x81)), "?");

  EXPECT_TRUE(fsoeIsKnownCommand(FsoeCommand::Reset));
  EXPECT_FALSE(fsoeIsKnownCommand(static_cast<FsoeCommand>(0x00)));
}

}  // namespace
}  // namespace mm::etg
