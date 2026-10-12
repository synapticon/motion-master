#include "comm/esc_diagnostics.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>

namespace {

using mm::comm::decodeDcSync;
using mm::comm::decodeSlaveDiagnostics;
using mm::comm::decodeWatchdog;
using mm::comm::watchdogTicks;

TEST(EscDiagnostics, DecodesPortsAndCounters) {
  // Port 0: link, communication. Port 1: link, loop closed.
  const uint16_t dlStatus = (1u << 4) | (1u << 9) | (1u << 5) | (1u << 10);
  std::array<uint8_t, mm::comm::kErrorCounterBytes> errors{};
  errors[0] = 1;   // port 0 invalid frame
  errors[3] = 2;   // port 1 RX error
  errors[9] = 3;   // port 1 forwarded error
  errors[12] = 4;  // processing unit
  errors[13] = 5;  // PDI
  errors[17] = 6;  // port 1 lost link
  const std::array<uint8_t, mm::comm::kWatchdogCounterBytes> watchdogs{0, 0, 7, 8};

  const auto d = decodeSlaveDiagnostics(3, dlStatus, errors, watchdogs);
  EXPECT_EQ(d.slavePosition, 3);
  EXPECT_TRUE(d.ports[0].linkUp);
  EXPECT_TRUE(d.ports[0].communication);
  EXPECT_FALSE(d.ports[0].loopClosed);
  EXPECT_EQ(d.ports[0].invalidFrame, 1);
  EXPECT_TRUE(d.ports[1].linkUp);
  EXPECT_TRUE(d.ports[1].loopClosed);
  EXPECT_EQ(d.ports[1].rxError, 2);
  EXPECT_EQ(d.ports[1].forwardedError, 3);
  EXPECT_EQ(d.ports[1].lostLink, 6);
  EXPECT_FALSE(d.ports[2].linkUp);
  EXPECT_EQ(d.processingUnitError, 4);
  EXPECT_EQ(d.pdiError, 5);
  EXPECT_EQ(d.processDataWatchdog, 7);
  EXPECT_EQ(d.pdiWatchdog, 8);
}

TEST(EscDiagnostics, DcSyncSignBitMeansBehind) {
  // Delay 0x00000064 (100 ns); difference 0x80000032: behind by 50 ns.
  const std::array<uint8_t, mm::comm::kDcSyncBytes> registers{0x64, 0, 0, 0, 0x32, 0, 0, 0x80};
  const auto d = decodeDcSync(2, false, registers);
  EXPECT_TRUE(d.dcCapable);
  EXPECT_EQ(d.propagationDelay, 100);
  EXPECT_EQ(d.systemTimeDifference, -50);

  const std::array<uint8_t, mm::comm::kDcSyncBytes> ahead{0, 0, 0, 0, 0x32, 0, 0, 0};
  EXPECT_EQ(decodeDcSync(2, true, ahead).systemTimeDifference, 50);
}

TEST(EscDiagnostics, WatchdogTickIsFortyNanosecondsTimesDividerPlusTwo) {
  // Divider 2498 gives the common 100 µs tick.
  const auto config = decodeWatchdog(2498, 1000, 1);
  EXPECT_TRUE(config.enabled);
  EXPECT_TRUE(config.running);
  EXPECT_EQ(config.timeout, std::chrono::milliseconds(100));
  EXPECT_FALSE(decodeWatchdog(2498, 0, 0).enabled);
}

TEST(EscDiagnostics, WatchdogTicksRoundFloorAndRefuse) {
  using std::chrono::microseconds;
  using std::chrono::nanoseconds;
  EXPECT_EQ(watchdogTicks(2498, microseconds(100'000)).value(), 1000);
  EXPECT_EQ(watchdogTicks(2498, microseconds(149)).value(), 1);  // rounds to nearest tick
  EXPECT_EQ(watchdogTicks(2498, nanoseconds(1)).value(), 1);     // never rounds to "disabled"
  EXPECT_EQ(watchdogTicks(2498, nanoseconds(0)).value(), 0);     // zero disables
  EXPECT_FALSE(watchdogTicks(2498, nanoseconds(-1)).has_value());
  EXPECT_FALSE(watchdogTicks(0, std::chrono::seconds(1)).has_value());  // 80 ns ticks overflow
}

}  // namespace
