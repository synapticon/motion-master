#include "node/parameter_refresher.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "node/device.h"
#include "node/device_manager.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::OdEntry;
using mm::comm::OdRead;
using mm::comm::SlaveInfo;
using mm::node::DeviceManager;
using mm::node::ParameterRefresher;

constexpr uint16_t kU32 = 0x0007;
constexpr uint16_t kPreOp = static_cast<uint16_t>(EtherCatState::PreOp);

std::vector<uint8_t> u32le(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}

/// Driver double: scan() reports @c slaves devices, each with the programmed object dictionary.
/// readSdo answers from a programmed map, after the per-drive delay. It records every read with its
/// drive and its start and end time, so a test can check how the refresher used each drive.
/// slaveState() is settable to bring the devices online.
class FakeDriver : public FieldbusDriver {
 public:
  /// One readSdo call: the drive it addressed, and when it started and ended.
  struct Read {
    uint16_t position = 0;
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point end;
  };

  // Programmed before any thread starts, and only read afterwards.
  uint16_t state = 0;
  int slaves = 1;
  std::vector<OdEntry> ods;
  std::map<uint32_t, std::vector<uint8_t>> reads;
  std::map<uint16_t, std::chrono::milliseconds> delays;  // per drive; no entry is no delay

  std::atomic<int> totalReads{0};
  std::atomic<int> maxInFlight{0};

  static uint32_t key(uint16_t index, uint8_t subindex) {
    return (static_cast<uint32_t>(index) << 8) | subindex;
  }
  void programOd(uint16_t index, uint8_t subindex, uint16_t dataType) {
    OdEntry e{};
    e.index = index;
    e.subindex = subindex;
    e.dataType = dataType;
    ods.push_back(e);
  }
  void programRead(uint16_t index, uint8_t subindex, std::vector<uint8_t> bytes) {
    reads[key(index, subindex)] = std::move(bytes);
  }
  int readCount(uint16_t index, uint8_t subindex) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = readCounts_.find(key(index, subindex));
    return it == readCounts_.end() ? 0 : it->second;
  }
  /// Every read of drive @p position, in the order they started.
  std::vector<Read> readsOf(uint16_t position) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Read> result;
    for (const Read& read : log_) {
      if (read.position == position) {
        result.push_back(read);
      }
    }
    return result;
  }
  /// The most reads of one drive that were in flight at the same time.
  int maxInFlightOnOneDrive() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return maxInFlightOnOneDrive_;
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t position, uint16_t index,
                                                           uint8_t subindex) override {
    const auto start = std::chrono::steady_clock::now();
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      ++readCounts_[key(index, subindex)];
      maxInFlightOnOneDrive_ = std::max(maxInFlightOnOneDrive_, ++inFlightByDrive_[position]);
    }
    const int inFlight = inFlight_.fetch_add(1) + 1;
    int seen = maxInFlight.load();
    while (inFlight > seen && !maxInFlight.compare_exchange_weak(seen, inFlight)) {
    }
    totalReads.fetch_add(1, std::memory_order_relaxed);

    if (const auto delay = delays.find(position); delay != delays.end()) {
      std::this_thread::sleep_for(delay->second);
    }

    inFlight_.fetch_sub(1);
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      --inFlightByDrive_[position];
      log_.push_back(
          Read{.position = position, .start = start, .end = std::chrono::steady_clock::now()});
    }
    auto it = reads.find(key(index, subindex));
    if (it == reads.end()) {
      return std::unexpected("no such object");
    }
    return it->second;
  }
  std::expected<OdRead, std::string> readObjectDictionary(uint16_t) override {
    return OdRead{.entries = ods};
  }
  uint16_t slaveState(uint16_t) const override { return state; }
  // CoE-capable stand-in: parameters are enumerated over the object dictionary, not SII.
  uint16_t mailboxProtocols(uint16_t) const override {
    return mm::comm::MailboxConfig::kProtocolCoe;
  }
  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override {
    return std::vector<SlaveStateRaw>(positions.size(),
                                      SlaveStateRaw{.alStatus = state, .alStatusCode = 0});
  }

  // --- unused stubs ---------------------------------------------------------
  std::expected<void, std::string> init() override { return {}; }
  std::expected<int, std::string> scan() override { return slaves; }
  SlaveInfo slaveInfo(uint16_t) const override { return {}; }
  std::expected<void, std::string> configureProcessData() override { return {}; }
  mm::comm::PdoLayout processDataLayout() override { return {}; }
  std::vector<mm::comm::SlaveConfig> busConfig() const override { return {}; }
  int exchangeProcessData(std::span<const uint8_t>, std::span<uint8_t>) override { return 0; }
  void stop() override {}
  std::expected<std::vector<mm::comm::SlaveDiagnostics>, std::string> readDiagnostics(
      const std::vector<uint16_t>&) override {
    return {};
  }
  std::expected<std::vector<mm::comm::DcSyncDiagnostics>, std::string> readDcSync(
      const std::vector<uint16_t>&) override {
    return {};
  }
  std::expected<void, std::string> writeSdo(uint16_t, uint16_t, uint8_t,
                                            std::span<const uint8_t>) override {
    return {};
  }
  std::expected<std::vector<uint8_t>, mm::comm::FoeError> readFile(
      uint16_t, const std::string&, std::chrono::milliseconds) override {
    return std::vector<uint8_t>{};
  }
  std::expected<void, mm::comm::FoeError> writeFile(uint16_t, const std::string&,
                                                    std::span<const uint8_t>) override {
    return {};
  }
  std::expected<void, std::string> readRegister(uint16_t, uint16_t, std::span<uint8_t>) override {
    return {};
  }
  std::expected<void, std::string> writeRegister(uint16_t, uint16_t,
                                                 std::span<const uint8_t>) override {
    return {};
  }
  void transitionToState(const std::vector<uint16_t>&, std::optional<EtherCatState>, EtherCatState,
                         std::chrono::steady_clock::duration, std::chrono::steady_clock::duration,
                         std::function<void()>, std::function<bool()>) override {}

 private:
  mutable std::mutex mutex_;  // guards the members below
  std::map<uint32_t, int> readCounts_;
  std::map<uint16_t, int> inFlightByDrive_;
  int maxInFlightOnOneDrive_ = 0;
  std::vector<Read> log_;
  std::atomic<int> inFlight_{0};
};

/// Brings @p dm up with @p slaves online devices. Each has U32 parameters 0x2030:01 to 0x2030:03,
/// and each reads back @p value. Returns the live driver pointer for per-test tweaks.
/// (DeviceManager is non-movable — it owns a shared_mutex — so it is built in place and taken by
/// reference.)
FakeDriver* setUp(DeviceManager& dm, uint32_t value, int slaves = 1) {
  auto driver = std::make_unique<FakeDriver>();
  FakeDriver* raw = driver.get();
  raw->state = kPreOp;
  raw->slaves = slaves;
  for (uint8_t subindex = 1; subindex <= 3; ++subindex) {
    raw->programOd(0x2030, subindex, kU32);
    raw->programRead(0x2030, subindex, u32le(value));
  }
  EXPECT_TRUE(dm.init(std::move(driver)).has_value());
  EXPECT_TRUE(dm.scan().has_value());
  for (int position = 1; position <= slaves; ++position) {
    EXPECT_TRUE(dm.initializeDeviceParameters(static_cast<uint16_t>(position), /*readValues=*/false)
                    .has_value());
  }
  return raw;
}

/// Runs the refresher's workers for @p duration, then stops them.
void runFor(ParameterRefresher& refresher, std::chrono::milliseconds duration) {
  refresher.start();
  std::this_thread::sleep_for(duration);
  refresher.stop();
}

TEST(ParameterRefresherTest, PollsDueParameterViaSdo) {
  DeviceManager dm;
  FakeDriver* driver = setUp(dm, 42);
  ParameterRefresher refresher(dm);

  refresher.acquire(1, 0x2030, 0x01, std::chrono::milliseconds{50});
  EXPECT_EQ(refresher.trackedCount(), 1u);

  refresher.pollDue();  // a freshly-acquired entry is due immediately
  EXPECT_EQ(driver->readCount(0x2030, 0x01), 1);
}

// What the polling is *for*: the value has to land where a cyclic task reads it. The refresher
// polls through the same path a control-plane read takes, so the object's cell is filled and
// Device::value<T>() serves it with the identical call a PDO-mapped object would use — which is the
// whole point of keeping the two indistinguishable.
TEST(ParameterRefresherTest, PollStoresIntoTheCellACyclicTaskReads) {
  DeviceManager dm;
  setUp(dm, 57);
  ParameterRefresher refresher(dm);

  const mm::node::Device* device = dm.findDevice(1);
  ASSERT_NE(device, nullptr);
  EXPECT_EQ(device->value<uint32_t>(0x2030, 0x01), std::optional<uint32_t>{0});  // never read yet

  refresher.acquire(1, 0x2030, 0x01, std::chrono::milliseconds{50});
  refresher.pollDue();
  EXPECT_EQ(device->value<uint32_t>(0x2030, 0x01), std::optional<uint32_t>{57});
}

TEST(ParameterRefresherTest, RefcountTracksAcquireAndRelease) {
  DeviceManager dm;
  setUp(dm, 7);
  ParameterRefresher refresher(dm);

  refresher.acquire(1, 0x2030, 0x01, std::chrono::milliseconds{50});
  refresher.acquire(1, 0x2030, 0x01,
                    std::chrono::milliseconds{50});  // a second monitoring wants the same object
  EXPECT_EQ(refresher.trackedCount(), 1u);

  refresher.release(1, 0x2030, 0x01);  // one still needs it
  EXPECT_EQ(refresher.trackedCount(), 1u);

  refresher.release(1, 0x2030, 0x01);  // last reference gone
  EXPECT_EQ(refresher.trackedCount(), 0u);

  refresher.release(1, 0x2030, 0x01);  // extra release is ignored
  EXPECT_EQ(refresher.trackedCount(), 0u);
}

TEST(ParameterRefresherTest, ReleaseStopsPolling) {
  DeviceManager dm;
  FakeDriver* driver = setUp(dm, 1);
  ParameterRefresher refresher(dm);

  refresher.acquire(1, 0x2030, 0x01, std::chrono::milliseconds{50});
  refresher.pollDue();
  EXPECT_EQ(driver->readCount(0x2030, 0x01), 1);

  refresher.release(1, 0x2030, 0x01);
  EXPECT_EQ(refresher.trackedCount(), 0u);

  refresher.pollDue();  // nothing tracked → no further bus access
  EXPECT_EQ(driver->readCount(0x2030, 0x01), 1);
}

TEST(ParameterRefresherTest, BackgroundThreadPolls) {
  DeviceManager dm;
  FakeDriver* driver = setUp(dm, 99);
  ParameterRefresher refresher(dm);

  refresher.acquire(1, 0x2030, 0x01, std::chrono::milliseconds{10});
  refresher.start();

  // Wait (bounded) for the background thread to poll at least once.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (driver->totalReads.load(std::memory_order_relaxed) == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  refresher.stop();

  EXPECT_GE(driver->totalReads.load(std::memory_order_relaxed), 1);
}

// One pass reads at most one object per drive: a read closes its drive for as long as it took.
TEST(ParameterRefresherTest, AReadClosesItsDriveForThePass) {
  DeviceManager dm;
  FakeDriver* driver = setUp(dm, 3, /*slaves=*/2);
  driver->delays[1] = std::chrono::milliseconds{5};
  ParameterRefresher refresher(dm);

  refresher.acquire(1, 0x2030, 0x01, std::chrono::milliseconds{50});
  refresher.acquire(1, 0x2030, 0x02, std::chrono::milliseconds{50});
  refresher.acquire(2, 0x2030, 0x01, std::chrono::milliseconds{50});
  refresher.pollDue();

  EXPECT_EQ(driver->readsOf(1).size(), 1u) << "the second object of drive 1 waits for the gap";
  EXPECT_EQ(driver->readsOf(2).size(), 1u) << "drive 2 has its own gate";

  // Once the gap has passed, the other object of drive 1 is read.
  std::this_thread::sleep_for(std::chrono::milliseconds{10});
  refresher.pollDue();
  EXPECT_EQ(driver->readsOf(1).size(), 2u);
  EXPECT_EQ(driver->readCount(0x2030, 0x02), 1);
}

// The refresher uses at most half of a drive's control-plane time. After each read, the next read
// of that drive starts no sooner than the read's own duration after it ended.
TEST(ParameterRefresherTest, LeavesAGapAsLongAsEachRead) {
  DeviceManager dm;
  FakeDriver* driver = setUp(dm, 4);
  driver->delays[1] = std::chrono::milliseconds{20};
  ParameterRefresher refresher(dm);

  for (uint8_t subindex = 1; subindex <= 3; ++subindex) {
    refresher.acquire(1, 0x2030, subindex, std::chrono::milliseconds{10});
  }
  runFor(refresher, std::chrono::milliseconds{500});

  const auto reads = driver->readsOf(1);
  ASSERT_GE(reads.size(), 3u);
  for (std::size_t i = 1; i < reads.size(); ++i) {
    const auto duration = reads[i - 1].end - reads[i - 1].start;
    EXPECT_GE(reads[i].start - reads[i - 1].end, duration) << "read " << i;
  }
  EXPECT_EQ(driver->maxInFlightOnOneDrive(), 1);
}

// Two drives are read at the same time, by different workers.
TEST(ParameterRefresherTest, ReadsTwoDrivesAtTheSameTime) {
  DeviceManager dm;
  FakeDriver* driver = setUp(dm, 5, /*slaves=*/2);
  driver->delays[1] = std::chrono::milliseconds{50};
  driver->delays[2] = std::chrono::milliseconds{50};
  ParameterRefresher refresher(dm);

  refresher.acquire(1, 0x2030, 0x01, std::chrono::milliseconds{10});
  refresher.acquire(2, 0x2030, 0x01, std::chrono::milliseconds{10});
  runFor(refresher, std::chrono::milliseconds{300});

  EXPECT_EQ(driver->maxInFlight.load(), 2);
  EXPECT_EQ(driver->maxInFlightOnOneDrive(), 1);
}

// A drive that takes long to answer delays only its own objects. Here drive 1 answers after
// 300 ms, close to a mailbox timeout, and drive 2 at once.
TEST(ParameterRefresherTest, ASlowDriveDoesNotDelayAnother) {
  DeviceManager dm;
  FakeDriver* driver = setUp(dm, 6, /*slaves=*/2);
  driver->delays[1] = std::chrono::milliseconds{300};
  ParameterRefresher refresher(dm);

  refresher.acquire(1, 0x2030, 0x01, std::chrono::milliseconds{10});
  refresher.acquire(2, 0x2030, 0x01, std::chrono::milliseconds{10});
  runFor(refresher, std::chrono::milliseconds{1000});

  // At a 10 ms period drive 2 is read about 100 times. One thread for both drives would read it
  // about twice, once after each slow read of drive 1.
  EXPECT_GE(driver->readsOf(2).size(), 20u);
  EXPECT_LE(driver->readsOf(1).size(), 3u);
}

// No more reads are in flight than the pool has workers, however many drives are due.
TEST(ParameterRefresherTest, NoMoreReadsInFlightThanWorkers) {
  DeviceManager dm;
  FakeDriver* driver = setUp(dm, 7, /*slaves=*/8);
  for (uint16_t position = 1; position <= 8; ++position) {
    driver->delays[position] = std::chrono::milliseconds{30};
  }
  ParameterRefresher refresher(dm);

  for (uint16_t position = 1; position <= 8; ++position) {
    refresher.acquire(position, 0x2030, 0x01, std::chrono::milliseconds{10});
  }
  runFor(refresher, std::chrono::milliseconds{300});

  EXPECT_LE(driver->maxInFlight.load(), 4);
  EXPECT_GE(driver->maxInFlight.load(), 2) << "the workers read in parallel";
  EXPECT_EQ(driver->maxInFlightOnOneDrive(), 1);
}

}  // namespace
