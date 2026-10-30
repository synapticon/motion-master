#include "comm/foe_error.h"

#include <gtest/gtest.h>

namespace mm::comm {
namespace {

TEST(MakeFoeRefusalTest, NotFoundIsTheKindACallerBranchesOn) {
  const FoeError error = makeFoeRefusal(0x8001, "Not found", "FOEread", 1, "x.bin");
  EXPECT_EQ(error.kind, FoeErrorKind::FileNotFound);
  EXPECT_EQ(error.retry, Retry::Permanent);
  // The text repeats the name, whatever its case, so it is left out.
  EXPECT_EQ(error.message, "FOEread slave 1 'x.bin' failed (FoE error 0x8001: Not found)");
  EXPECT_EQ(makeFoeRefusal(0x8001, "NOT FOUND", "FOEread", 1, "x.bin").message, error.message);
}

TEST(MakeFoeRefusalTest, KeepsATextThatSaysMore) {
  // What an Integro sends for a missing file.
  const FoeError error = makeFoeRefusal(0x8001, "Path not found", "FOEread", 1, "x.bin");
  EXPECT_EQ(error.message,
            "FOEread slave 1 'x.bin' failed (FoE error 0x8001: Not found, \"Path not found\")");
}

TEST(MakeFoeRefusalTest, EveryOtherCodeIsAProtocolErrorWithTheCodeInTheMessage) {
  const FoeError denied = makeFoeRefusal(0x8002, "", "FOEwrite", 2, "x.bin");
  EXPECT_EQ(denied.kind, FoeErrorKind::Protocol);
  EXPECT_EQ(denied.message, "FOEwrite slave 2 'x.bin' failed (FoE error 0x8002: Access denied)");

  // A code outside the table keeps its number and the slave's text.
  const FoeError unknown = makeFoeRefusal(3, "Packet Number Wrong", "FOEwrite", 2, "x.bin");
  EXPECT_EQ(unknown.kind, FoeErrorKind::Protocol);
  EXPECT_EQ(unknown.message,
            "FOEwrite slave 2 'x.bin' failed (FoE error 0x0003, \"Packet Number Wrong\")");
}

TEST(IsTransientReadRefusalTest, OnlyIllegalAndProgramErrorAreAskedAgain) {
  EXPECT_TRUE(isTransientReadRefusal(0x8004));
  EXPECT_TRUE(isTransientReadRefusal(0x800B));
  EXPECT_FALSE(isTransientReadRefusal(0x8001));
  EXPECT_FALSE(isTransientReadRefusal(0x8002));
  EXPECT_FALSE(isTransientReadRefusal(0x8000));
  EXPECT_FALSE(isTransientReadRefusal(0));
}

}  // namespace
}  // namespace mm::comm
