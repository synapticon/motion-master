#include "comm/soem_mailbox.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using mm::comm::soem::makeFoeAbortRequest;

// The abort is the one message a SOMANET bootloader accepts while it is stuck in an FoE read, so
// its bytes must be an FoE ERROR request exactly as ETG.1000.6 lays it out. A wrong length or type
// makes the slave answer with a mailbox error and stay stuck.
TEST(FoeAbortRequest, IsAnFoeErrorRequest) {
  const std::vector<uint8_t> request = makeFoeAbortRequest(3);
  const std::string text = "Aborted by the master";

  ASSERT_EQ(request.size(), 6U + 6U + text.size());

  // Mailbox header. The length counts the FoE header and the text, not the mailbox header.
  EXPECT_EQ(request[0] | (request[1] << 8), static_cast<int>(6 + text.size()));
  EXPECT_EQ(request[2], 0x00);  // Address
  EXPECT_EQ(request[3], 0x00);
  EXPECT_EQ(request[4], 0x00);  // Channel and priority
  EXPECT_EQ(request[5], ECT_MBXT_FOE | (3 << 4));

  // FoE header: opcode ERROR, reserved byte, error code 0x8000 little-endian.
  EXPECT_EQ(request[6], 0x05);
  EXPECT_EQ(request[7], 0x00);
  EXPECT_EQ(request[8], 0x00);
  EXPECT_EQ(request[9], 0x80);
  EXPECT_EQ(request[10], 0x00);
  EXPECT_EQ(request[11], 0x00);

  EXPECT_EQ(std::string(request.begin() + 12, request.end()), text);
}

// The mailbox counter has three bits in the header. A counter that spilled into the type nibble's
// neighbour would change the message type.
TEST(FoeAbortRequest, KeepsTheCounterInThreeBits) {
  EXPECT_EQ(makeFoeAbortRequest(7)[5], ECT_MBXT_FOE | (7 << 4));
  EXPECT_EQ(makeFoeAbortRequest(9)[5], ECT_MBXT_FOE | (1 << 4));
}

}  // namespace
