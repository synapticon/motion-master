#include "comm/soem_fieldbus_driver.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

using mm::comm::soem::SoemFieldbusDriver;
using mm::comm::soem::SoemFieldbusDriverConfig;

// What these cover, and what they deliberately cannot.
//
// slaveState() answers from a lock-free mirror rather than from SOEM's slavelist, so that a reader
// does not wait on controlPlaneMutex_ — which a firmware transfer holds for the whole multi-second
// FoE write, and which the monitoring sampler would otherwise queue behind on every flush. The half
// of that mechanism reachable without a NIC is the one tested here: the mirror is allocated and
// zeroed by the constructor, indexing it is bounded, and tearing the context down republishes
// rather than leaving stale states readable.
//
// The other half — that a transition publishes each intermediate state as it goes — needs a real
// context, since publishSlaveStates() copies out of ctx_->slavelist. It is verified on hardware by
// monitoring one device while another is flashed: the stream must keep flushing throughout.
//
// An interface name is required but never opened: only init() touches the network, and no test here
// calls it. That is what makes the driver constructible in a unit test at all.
SoemFieldbusDriver makeDriver() {
  return SoemFieldbusDriver(
      SoemFieldbusDriverConfig{.ifname = "test0", .mailboxStatusFmmu = false});
}

TEST(SoemFieldbusDriverState, ReportsNoStateBeforeInit) {
  // Zero is "no state known" — the value every consumer already treats as offline. The point is
  // that it comes from a value-initialised mirror rather than from an unread slavelist, so a driver
  // that never scanned cannot report a device as sitting in some state.
  SoemFieldbusDriver driver = makeDriver();
  for (uint16_t position = 0; position < 8; ++position) {
    EXPECT_EQ(driver.slaveState(position), 0) << "position " << position;
  }
}

TEST(SoemFieldbusDriverState, BoundsAnOutOfRangePosition) {
  // The interface lets an implementation trust the position, and the mirror is sized to SOEM's
  // compile-time maximum — so a position past it must read as "no state known" rather than off the
  // end of the array. Cheap to guarantee (one comparison against an immutable size) and the kind of
  // read that would otherwise be a silent out-of-bounds.
  SoemFieldbusDriver driver = makeDriver();
  EXPECT_EQ(driver.slaveState(60000), 0);
  EXPECT_EQ(driver.slaveState(UINT16_MAX), 0);
}

TEST(SoemFieldbusDriverState, StillReportsNoStateAfterStop) {
  // stop() destroys the context, so anything the mirror held describes a bus that is gone. Reading
  // the last-known states of a torn-down bus would show devices apparently still in OP, which is
  // why closeContext() republishes instead of simply leaving the mirror alone.
  SoemFieldbusDriver driver = makeDriver();
  driver.stop();
  EXPECT_EQ(driver.slaveState(1), 0);
  // Idempotent — the destructor calls closeContext() again, and a second publish must be harmless.
  driver.stop();
  EXPECT_EQ(driver.slaveState(1), 0);
}

TEST(SoemFieldbusDriverState, IsReadableConcurrently) {
  // The whole purpose of the mirror is that this read is safe and unsynchronised, so exercise it as
  // such: many threads reading every position while another tears the context down (the one
  // operation available without a NIC that publishes). Under TSan this is the test that would fail
  // if slaveState() ever went back to touching slavelist without a lock; without TSan it still
  // catches a crash from an unallocated or wrongly sized mirror.
  SoemFieldbusDriver driver = makeDriver();
  std::vector<std::thread> readers;
  for (int reader = 0; reader < 4; ++reader) {
    readers.emplace_back([&driver] {
      for (int pass = 0; pass < 200; ++pass) {
        for (uint16_t position = 0; position < 32; ++position) {
          EXPECT_EQ(driver.slaveState(position), 0);
        }
      }
    });
  }
  driver.stop();
  for (auto& reader : readers) {
    reader.join();
  }
}

}  // namespace
