#include "node/device_manager.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::OdEntry;
using mm::comm::SlaveInfo;
using mm::node::DeviceManager;

/// Minimal FieldbusDriver test double. init() returns a configurable result;
/// scan() reports a configurable slave count. Every other method is a trivial
/// stub — the lifecycle tests below never reach them.
class FakeDriver : public FieldbusDriver {
 public:
  explicit FakeDriver(bool initSucceeds, int slaves = 1)
      : initSucceeds_(initSucceeds), slaves_(slaves) {}

  std::expected<void, std::string> init() override {
    if (!initSucceeds_) {
      return std::unexpected("fake init failure");
    }
    return {};
  }

  std::expected<int, std::string> scan() override { return slaves_; }

  SlaveInfo slaveInfo(uint16_t) const override { return {}; }
  void exchangeProcessData() override {}
  void stop() override {}

  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override {
    return std::vector<SlaveStateRaw>(positions.size(), SlaveStateRaw{});
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t, uint8_t) override {
    return std::vector<uint8_t>{};
  }

  std::expected<void, std::string> writeSdo(uint16_t, uint16_t, uint8_t,
                                            std::span<const uint8_t>) override {
    return {};
  }

  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    return std::vector<OdEntry>{};
  }

  std::expected<std::vector<uint8_t>, std::string> readFile(uint16_t, const std::string&) override {
    return std::vector<uint8_t>{};
  }

  std::expected<void, std::string> writeFile(uint16_t, const std::string&,
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
  bool initSucceeds_;
  int slaves_;
};

TEST(DeviceManagerInit, SuccessfulInitMarksInitialised) {
  DeviceManager dm;
  EXPECT_FALSE(dm.initialised());

  auto result = dm.init(std::make_unique<FakeDriver>(true));
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(dm.initialised());
}

TEST(DeviceManagerInit, FailedInitLeavesUninitialised) {
  DeviceManager dm;

  auto result = dm.init(std::make_unique<FakeDriver>(false));
  ASSERT_FALSE(result.has_value());
  // The half-dead driver must be dropped — otherwise initialised() lies and the
  // next control-plane call dereferences a context that never opened.
  EXPECT_FALSE(dm.initialised());
}

TEST(DeviceManagerInit, ScanAfterFailedInitReturnsErrorNotCrash) {
  DeviceManager dm;
  (void)dm.init(std::make_unique<FakeDriver>(false));

  // Regression: previously scan() ran on a retained driver whose context was
  // null and segfaulted. It must now fail cleanly.
  auto scan = dm.scan();
  EXPECT_FALSE(scan.has_value());
}

TEST(DeviceManagerInit, InitCanBeRetriedAfterFailureWithoutReset) {
  DeviceManager dm;
  (void)dm.init(std::make_unique<FakeDriver>(false));
  ASSERT_FALSE(dm.initialised());

  auto retry = dm.init(std::make_unique<FakeDriver>(true));
  EXPECT_TRUE(retry.has_value());
  EXPECT_TRUE(dm.initialised());
}

TEST(DeviceManagerInit, SecondInitWhileInitialisedIsRejected) {
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::make_unique<FakeDriver>(true)).has_value());

  // One-shot contract: re-init must be rejected (caller must reset() first) so a
  // live driver is never replaced out from under the Devices referencing it.
  auto again = dm.init(std::make_unique<FakeDriver>(true));
  EXPECT_FALSE(again.has_value());
  EXPECT_TRUE(dm.initialised());

  dm.reset();
  EXPECT_FALSE(dm.initialised());
  EXPECT_TRUE(dm.init(std::make_unique<FakeDriver>(true)).has_value());
}

}  // namespace
