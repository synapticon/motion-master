#include <gtest/gtest.h>

#include <vector>

#include "comm/igh_fieldbus_driver.h"

namespace {

using mm::comm::igh::buildIghLayout;
using mm::comm::igh::IghSlaveWindow;

TEST(IghLayout, DomainsBecomeTheImagesAndEachSlaveKeepsItsOffsets) {
  const std::vector<IghSlaveWindow> windows{
      {.slavePosition = 1,
       .hasOutputs = true,
       .outputOffset = 0,
       .outputBits = 96,
       .hasInputs = true,
       .inputOffset = 0,
       .inputBits = 200},
      {.slavePosition = 2,
       .hasOutputs = true,
       .outputOffset = 12,
       .outputBits = 32,
       .hasInputs = true,
       .inputOffset = 25,
       .inputBits = 64},
  };
  const auto layout = buildIghLayout(windows, 16, 33);
  EXPECT_EQ(layout.outputBytes, 16U);
  EXPECT_EQ(layout.inputBytes, 33U);
  ASSERT_EQ(layout.slaves.size(), 2U);
  EXPECT_EQ(layout.slaves[1].slavePosition, 2);
  EXPECT_EQ(layout.slaves[1].outputOffset, 12U);
  EXPECT_EQ(layout.slaves[1].outputBytes, 4U);
  EXPECT_EQ(layout.slaves[1].inputOffset, 25U);
  EXPECT_EQ(layout.slaves[1].inputBytes, 8U);
  // Two slaves, each with outputs (2) and inputs (1).
  EXPECT_EQ(layout.expectedWkc, 6);
}

TEST(IghLayout, ADirectionUnderOneByteReportsNoBytesButStillCounts) {
  const std::vector<IghSlaveWindow> windows{
      {.slavePosition = 1,
       .hasOutputs = true,
       .outputOffset = 4,
       .outputBits = 4,
       .hasInputs = false},
  };
  const auto layout = buildIghLayout(windows, 1, 0);
  ASSERT_EQ(layout.slaves.size(), 1U);
  EXPECT_EQ(layout.slaves[0].outputBytes, 0U);
  EXPECT_EQ(layout.slaves[0].outputOffset, 0U);
  EXPECT_EQ(layout.expectedWkc, 2);
}

}  // namespace
