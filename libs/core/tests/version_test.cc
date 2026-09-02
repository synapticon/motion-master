#include "core/version.h"

#include <gtest/gtest.h>

#include <semver.hpp>
#include <string>

TEST(VersionTest, StringConstant) { EXPECT_EQ(mm::core::kVersion, "6.0.0-alpha.86"); }

TEST(VersionTest, SemverComponents) {
  semver::version<> v{};
  semver::parse(mm::core::kVersion, v);
  EXPECT_EQ(v.major(), 6);
  EXPECT_EQ(v.minor(), 0);
  EXPECT_EQ(v.patch(), 0);
}

TEST(VersionTest, SemverRoundtrip) {
  semver::version<> v{};
  semver::parse(mm::core::kVersion, v);
  EXPECT_EQ(v.to_string(), std::string{mm::core::kVersion});
}
