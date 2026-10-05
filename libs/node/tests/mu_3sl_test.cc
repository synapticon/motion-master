#include <MU_3SL_interface.h>
#include <gtest/gtest.h>

#include <string_view>

// The version pins the vendored build. A test that fails here means the files in
// extern/MU_3SL-3.4.2 no longer match the directory name.
TEST(Mu3slTest, LoadsTheVendoredVersion) {
  EXPECT_EQ(MU_GetVersionMajor(), 3);
  EXPECT_EQ(MU_GetVersionMinor(), 4);
  EXPECT_EQ(MU_GetVersionPatch(), 2);
  EXPECT_EQ(std::string_view{MU_GetVersionString()}, "3.4.2.1");
}
