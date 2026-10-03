#include "comm/spoe_fieldbus_driver.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "fake_spoe_server.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::spoe::SpoeFieldbusDriver;
using mm::comm::spoe::SpoeFieldbusDriverConfig;
using mm::comm::spoe::SpoeMode;
using mm::comm::testing::FakeSpoeEntry;
using mm::comm::testing::FakeSpoeServer;
using mm::comm::testing::SpoeFault;
using std::chrono::milliseconds;

// The driver against the fake, which answers as the netX firmware does. Hardware behaviour the
// fake does not model is out of reach here, and #36 lists what still needs a drive.

SpoeFieldbusDriverConfig configFor(const FakeSpoeServer& server,
                                   SpoeMode mode = SpoeMode::kControl) {
  return SpoeFieldbusDriverConfig{.hosts = {"127.0.0.1"},
                                  .port = server.port(),
                                  .mode = mode,
                                  .watchdogMs = 75,
                                  .requestTimeout = milliseconds{300},
                                  .connectTimeout = milliseconds{300}};
}

std::vector<uint8_t> u32le(uint32_t value) {
  return {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
          static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
}

void setIdentity(FakeSpoeServer& server) {
  server.setObject(0x1018, 1, u32le(0x000022D2));
  server.setObject(0x1018, 2, u32le(0x00000201));
  server.setObject(0x1018, 3, u32le(0x00000A0C));
  server.setObject(0x1018, 4, u32le(12345));
  server.setObject(0x1008, 0, {'I', 'n', 't', 'e', 'g', 'r', 'o', 0});
}

FakeSpoeEntry entry(uint8_t objectCode, uint16_t bitLength, std::string name) {
  return FakeSpoeEntry{.dataType = 0x0007,
                       .objectCode = objectCode,
                       .bitLength = bitLength,
                       .access = 0x07,
                       .name = std::move(name)};
}

TEST(SpoeFieldbusDriver, InitNeedsAnAddress) {
  SpoeFieldbusDriver driver(SpoeFieldbusDriverConfig{});
  EXPECT_FALSE(driver.init().has_value());
}

TEST(SpoeFieldbusDriver, ScanReadsTheIdentityAndTheState) {
  FakeSpoeServer server;
  setIdentity(server);
  SpoeFieldbusDriver driver(configFor(server));
  ASSERT_TRUE(driver.init().has_value());
  ASSERT_EQ(driver.scan().value_or(0), 1);

  const auto info = driver.slaveInfo(1);
  EXPECT_EQ(info.vendorId, 0x22D2U);
  EXPECT_EQ(info.productCode, 0x0201U);
  EXPECT_EQ(info.revisionNumber, 0x0A0CU);
  EXPECT_EQ(info.serialNumber, 12345U);
  EXPECT_EQ(info.name, "Integro");
  EXPECT_EQ(driver.slaveState(1), static_cast<uint16_t>(EtherCatState::PreOp));
}

TEST(SpoeFieldbusDriver, ScanLeavesTheIdentityEmptyInInit) {
  // The firmware refuses every SDO in INIT and BOOT, the identity objects included.
  FakeSpoeServer server;
  setIdentity(server);
  server.setState(mm::comm::testing::kSpoeStateInit);
  SpoeFieldbusDriver driver(configFor(server));
  ASSERT_EQ(driver.scan().value_or(0), 1);
  EXPECT_EQ(driver.slaveInfo(1).vendorId, 0U);
  EXPECT_EQ(driver.slaveState(1), static_cast<uint16_t>(EtherCatState::Init));
}

TEST(SpoeFieldbusDriver, AnUnreachableDriveKeepsItsPosition) {
  // 127.0.0.2 is loopback where nothing listens. The second drive must stay at position 2.
  FakeSpoeServer server;
  setIdentity(server);
  auto config = configFor(server);
  config.hosts = {"127.0.0.2", "127.0.0.1"};
  SpoeFieldbusDriver driver(config);
  ASSERT_EQ(driver.scan().value_or(0), 2);
  EXPECT_EQ(driver.slaveState(1), 0);
  EXPECT_EQ(driver.slaveInfo(1).vendorId, 0U);
  EXPECT_EQ(driver.slaveInfo(2).vendorId, 0x22D2U);
}

TEST(SpoeFieldbusDriver, ReportsCoeAndFoe) {
  FakeSpoeServer server;
  SpoeFieldbusDriver driver(configFor(server));
  EXPECT_EQ(driver.mailboxProtocols(1), 0x04 | 0x08);
  EXPECT_EQ(driver.mailboxProtocols(2), 0);
}

TEST(SpoeFieldbusDriver, ReadsAndWritesAnSdo) {
  FakeSpoeServer server;
  server.setObject(0x6040, 0, {0x00, 0x00});
  SpoeFieldbusDriver driver(configFor(server));
  ASSERT_TRUE(driver.scan().has_value());
  const std::vector<uint8_t> value{0x0F, 0x00};
  ASSERT_TRUE(driver.writeSdo(1, 0x6040, 0, value).has_value());
  EXPECT_EQ(driver.readSdo(1, 0x6040, 0).value_or(std::vector<uint8_t>{}), value);
}

TEST(SpoeFieldbusDriver, NamesTheSdoStatus) {
  FakeSpoeServer server;
  SpoeFieldbusDriver driver(configFor(server));
  ASSERT_TRUE(driver.scan().has_value());
  const auto missing = driver.readSdo(1, 0x6042, 0);
  ASSERT_FALSE(missing.has_value());
  EXPECT_NE(missing.error().find("object not found"), std::string::npos) << missing.error();
}

TEST(SpoeFieldbusDriver, ReadStatesReadsTheDrive) {
  FakeSpoeServer server;
  SpoeFieldbusDriver driver(configFor(server));
  ASSERT_TRUE(driver.scan().has_value());
  server.setState(mm::comm::testing::kSpoeStateSafeOp);
  const auto states = driver.readStates({1});
  ASSERT_TRUE(states.has_value());
  EXPECT_EQ((*states)[0].alStatus, static_cast<uint16_t>(EtherCatState::SafeOp));
  EXPECT_EQ(driver.slaveState(1), static_cast<uint16_t>(EtherCatState::SafeOp));
}

TEST(SpoeFieldbusDriver, ChangesTheStateInControlMode) {
  FakeSpoeServer server;
  SpoeFieldbusDriver driver(configFor(server, SpoeMode::kControl));
  ASSERT_TRUE(driver.scan().has_value());
  EXPECT_FALSE(driver.stateChangeRefusal().has_value());
  driver.transitionToState({1}, std::nullopt, EtherCatState::SafeOp, milliseconds{500},
                           milliseconds{2000}, nullptr, nullptr);
  EXPECT_EQ(server.state(), mm::comm::testing::kSpoeStateSafeOp);
  EXPECT_EQ(driver.slaveState(1), static_cast<uint16_t>(EtherCatState::SafeOp));
}

TEST(SpoeFieldbusDriver, KeepsTheStateTheDriveReportsWhenItRefuses) {
  FakeSpoeServer server;
  server.setRefuseStateChanges(true);
  SpoeFieldbusDriver driver(configFor(server, SpoeMode::kControl));
  ASSERT_TRUE(driver.scan().has_value());
  driver.transitionToState({1}, std::nullopt, EtherCatState::SafeOp, milliseconds{500},
                           milliseconds{2000}, nullptr, nullptr);
  EXPECT_EQ(driver.slaveState(1), static_cast<uint16_t>(EtherCatState::PreOp));
}

TEST(SpoeFieldbusDriver, RefusesEveryStateChangeInMonitorMode) {
  // The firmware itself changes the state in Monitor mode too. The driver refuses, because a PLC
  // owns the state there.
  FakeSpoeServer server;
  SpoeFieldbusDriver driver(configFor(server, SpoeMode::kMonitor));
  ASSERT_TRUE(driver.scan().has_value());
  ASSERT_TRUE(driver.stateChangeRefusal().has_value());
  const int before = server.requestCount();
  driver.transitionToState({1}, std::nullopt, EtherCatState::SafeOp, milliseconds{500},
                           milliseconds{2000}, nullptr, nullptr);
  EXPECT_EQ(server.requestCount(), before);
  EXPECT_EQ(server.state(), mm::comm::testing::kSpoeStatePreOp);
}

TEST(SpoeFieldbusDriver, AClosedConnectionClearsTheState) {
  FakeSpoeServer server;
  SpoeFieldbusDriver driver(configFor(server));
  ASSERT_TRUE(driver.scan().has_value());
  server.dropClient();
  EXPECT_FALSE(driver.readSdo(1, 0x6041, 0).has_value());
  EXPECT_EQ(driver.slaveState(1), 0);
  // A rescan reconnects.
  ASSERT_TRUE(driver.scan().has_value());
  EXPECT_EQ(driver.slaveState(1), static_cast<uint16_t>(EtherCatState::PreOp));
}

TEST(SpoeFieldbusDriver, ReadsTheObjectDictionary) {
  FakeSpoeServer server;
  server.describeEntry(0x1000, 0, entry(0x07, 32, "Device type"));
  server.describeEntry(0x1018, 0, entry(0x09, 8, "Identity"));
  for (uint8_t sub = 1; sub <= 4; ++sub) {
    server.describeEntry(0x1018, sub, entry(0x07, 32, "Identity entry"));
  }
  for (uint16_t i = 0; i < 20; ++i) {
    server.describeEntry(static_cast<uint16_t>(0x2000 + i), 0, entry(0x07, 16, "Value"));
  }
  SpoeFieldbusDriver driver(configFor(server));
  ASSERT_TRUE(driver.scan().has_value());
  const auto od = driver.readObjectDictionary(1);
  ASSERT_TRUE(od.has_value()) << od.error();
  EXPECT_EQ(od->entries.size(), 26U);
  EXPECT_EQ(od->missingEntries, 0);
  EXPECT_EQ(od->entries[1].index, 0x1018);
  EXPECT_EQ(od->entries[1].objectCode, 0x09);
  EXPECT_EQ(od->entries[1].name, "Identity");
  EXPECT_EQ(od->entries[2].bitLength, 32);
}

TEST(SpoeFieldbusDriver, FillsTheEntriesOfALostParameterListPacket) {
  FakeSpoeServer server;
  for (uint16_t i = 0; i < 20; ++i) {
    server.describeEntry(static_cast<uint16_t>(0x2000 + i), 0, entry(0x07, 16, "Value"));
  }
  SpoeFieldbusDriver driver(configFor(server));
  ASSERT_TRUE(driver.scan().has_value());
  // The scan sent its requests, so the next ones are the index list and the first packet. The
  // fault must hit the first middle packet, which is the third request from here.
  server.injectFaultAfter(2, SpoeFault::kLoseParamListPacket);
  const int before = server.requestCount();
  const auto od = driver.readObjectDictionary(1);
  ASSERT_TRUE(od.has_value()) << od.error();
  EXPECT_EQ(od->entries.size(), 20U);
  EXPECT_EQ(od->missingEntries, 0);
  // The index list, the first packet, three middle packets of 7, 7 and 6 entries, and one
  // description for each of the 7 lost entries.
  EXPECT_EQ(server.requestCount() - before, 1 + 1 + 3 + 7);
}

TEST(SpoeFieldbusDriver, ASubindexTheDriveDoesNotHaveIsNotMissing) {
  // A record with subindices 1 and 3. The firmware walks 1 to 3 and sends a cleared description
  // for 2, which is an answer and not a loss.
  FakeSpoeServer server;
  server.describeEntry(0x2100, 0, entry(0x09, 8, "Record"));
  server.describeEntry(0x2100, 1, entry(0x07, 16, "First"));
  server.describeEntry(0x2100, 3, entry(0x07, 16, "Third"));
  SpoeFieldbusDriver driver(configFor(server));
  ASSERT_TRUE(driver.scan().has_value());
  const auto od = driver.readObjectDictionary(1);
  ASSERT_TRUE(od.has_value()) << od.error();
  EXPECT_EQ(od->entries.size(), 3U);
  EXPECT_EQ(od->missingEntries, 0);
}

TEST(SpoeFieldbusDriver, TheEscCallsAreNotSupported) {
  FakeSpoeServer server;
  SpoeFieldbusDriver driver(configFor(server));
  std::array<uint8_t, 2> bytes{};
  EXPECT_FALSE(driver.readRegister(1, 0x0130, bytes).has_value());
  EXPECT_FALSE(driver.readSii(1).has_value());
  EXPECT_FALSE(driver.readDiagnostics({1}).has_value());
  EXPECT_FALSE(driver.readDcSync({1}).has_value());
  EXPECT_TRUE(driver.busConfig().empty());
}

TEST(SpoeParameterEntries, DecodesTheFirmwareLayout) {
  std::vector<uint8_t> bytes(mm::comm::spoe::kParameterEntrySize, 0);
  bytes[0] = 0x18;
  bytes[1] = 0x10;
  bytes[2] = 0;
  bytes[4] = 0x05;
  bytes[6] = 0x09;
  bytes[8] = 8;
  bytes[10] = 0x07;
  bytes[12] = 4;
  const std::string name = "Identity";
  std::copy(name.begin(), name.end(), bytes.begin() + 16);
  // A cleared entry and a short tail are both skipped.
  bytes.resize(bytes.size() + mm::comm::spoe::kParameterEntrySize + 10, 0);

  const auto entries = mm::comm::spoe::decodeParameterEntries(bytes);
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].entry.index, 0x1018);
  EXPECT_EQ(entries[0].entry.dataType, 0x0005);
  EXPECT_EQ(entries[0].entry.objectCode, 0x09);
  EXPECT_EQ(entries[0].entry.bitLength, 8);
  EXPECT_EQ(entries[0].entry.access, 0x07);
  EXPECT_EQ(entries[0].entry.name, "Identity");
  EXPECT_EQ(entries[0].subindexCount, 4U);
}

}  // namespace
