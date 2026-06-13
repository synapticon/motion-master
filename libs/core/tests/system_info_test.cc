#include "core/system_info.h"

#include <gtest/gtest.h>

// collectSystemInfo() is platform-dependent and best-effort, so we cannot assert exact values.
// These checks pin down the contract that holds on every supported host: the call never throws,
// always reports at least one logical core, and a non-empty disk capacity covers its free space.

TEST(SystemInfoTest, ReportsAtLeastOneCore) {
  const auto info = mm::core::collectSystemInfo();
  EXPECT_GE(info.cpuCores, 1u);
}

TEST(SystemInfoTest, DiskFreeNotAboveTotal) {
  const auto info = mm::core::collectSystemInfo();
  if (info.diskTotalBytes > 0) {
    EXPECT_LE(info.diskFreeBytes, info.diskTotalBytes);
  }
}
