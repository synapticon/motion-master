#include "comm/spoe_frame.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using mm::comm::spoe::decodeHeader;
using mm::comm::spoe::encodeFrame;
using mm::comm::spoe::Frame;
using mm::comm::spoe::kHeaderSize;
using mm::comm::spoe::kMaxDataSize;

TEST(SpoeFrame, EncodesEveryHeaderFieldLittleEndian) {
  const auto bytes = encodeFrame(
      Frame{.type = 0x20, .sequenceId = 0x1234, .status = 0xABCD, .data = {0x01, 0x02}});
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(*bytes, (std::vector<uint8_t>{0x20, 0x34, 0x12, 0xCD, 0xAB, 0x02, 0x00, 0x01, 0x02}));
}

TEST(SpoeFrame, AcceptsDataUpToTheFirmwareBufferAndNoMore) {
  EXPECT_TRUE(encodeFrame(Frame{.type = 0x02,
                                .sequenceId = 1,
                                .status = 0,
                                .data = std::vector<uint8_t>(kMaxDataSize)})
                  .has_value());
  EXPECT_FALSE(encodeFrame(Frame{.type = 0x02,
                                 .sequenceId = 1,
                                 .status = 0,
                                 .data = std::vector<uint8_t>(kMaxDataSize + 1)})
                   .has_value());
}

TEST(SpoeFrame, DecodesAHeaderOnlyWhenAllSevenBytesAreThere) {
  const std::vector<uint8_t> bytes{0x0F, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x08};
  const auto header = decodeHeader(bytes);
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(header->type, 0x0F);
  EXPECT_EQ(header->sequenceId, 2);
  EXPECT_EQ(header->status, 1);
  EXPECT_EQ(header->dataLength, 1);
  EXPECT_FALSE(decodeHeader(std::vector<uint8_t>(kHeaderSize - 1)).has_value());
}

}  // namespace
