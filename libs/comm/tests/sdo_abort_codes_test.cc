#include "comm/sdo_abort_codes.h"

#include <gtest/gtest.h>

namespace mm::comm {
namespace {

TEST(IsNonexistentEntryAbortTest, AcceptsAMissingObjectAndAMissingSubindex) {
  EXPECT_TRUE(isNonexistentEntryAbort(0x06020000));
  EXPECT_TRUE(isNonexistentEntryAbort(0x06090011));
}

// Every other refusal can come from a fault that a second attempt does not have, so it keeps the
// retries.
TEST(IsNonexistentEntryAbortTest, RejectsEveryOtherCode) {
  for (const SdoAbortCode& abort : kSdoAbortCodes) {
    if (abort.code != 0x06020000 && abort.code != 0x06090011) {
      EXPECT_FALSE(isNonexistentEntryAbort(abort.code)) << std::hex << abort.code;
    }
  }
  EXPECT_FALSE(isNonexistentEntryAbort(0));
}

}  // namespace
}  // namespace mm::comm
