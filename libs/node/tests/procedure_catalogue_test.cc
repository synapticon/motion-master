#include "node/procedure_catalogue.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "node/device_manager.h"
#include "node/procedure_manager.h"
#include "node/synapticon.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::OdEntry;
using mm::comm::SlaveInfo;
using mm::node::cancelProcedure;
using mm::node::DeviceManager;
using mm::node::kSynapticonVendorId;
using mm::node::listProcedures;
using mm::node::ParameterType;
using mm::node::procedureCatalogue;
using mm::node::ProcedureError;
using mm::node::ProcedureManager;
using mm::node::procedureSnapshot;
using mm::node::ProcedureStatus;
using mm::node::startProcedure;

constexpr std::string_view kOsCommand = "os-command";

/// Driver double reporting a fixed slave count and a configurable vendor ID, which is all the
/// catalogue consults: applicability is decided from identity known at scan time, and no test here
/// runs a body to completion against the bus.
class VendorFakeDriver : public FieldbusDriver {
 public:
  VendorFakeDriver(int slaves, uint32_t vendorId) : slaves_(slaves), vendorId_(vendorId) {}

  std::expected<int, std::string> scan() override { return slaves_; }
  SlaveInfo slaveInfo(uint16_t) const override {
    SlaveInfo info{};
    info.vendorId = vendorId_;
    return info;
  }
  uint16_t slaveState(uint16_t) const override {
    return static_cast<uint16_t>(EtherCatState::PreOp);
  }

  // --- unused stubs ---------------------------------------------------------
  std::expected<void, std::string> init() override { return {}; }
  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    return std::vector<OdEntry>{};
  }
  std::expected<void, std::string> configureProcessData() override { return {}; }
  mm::comm::PdoLayout processDataLayout() override { return {}; }
  int exchangeProcessData(std::span<const uint8_t>, std::span<uint8_t>) override { return 0; }
  void stop() override {}
  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t, uint8_t) override {
    return std::vector<uint8_t>{};
  }
  std::expected<void, std::string> writeSdo(uint16_t, uint16_t, uint8_t,
                                            std::span<const uint8_t>) override {
    return {};
  }
  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override {
    return std::vector<SlaveStateRaw>(positions.size(), SlaveStateRaw{});
  }
  std::expected<std::vector<uint8_t>, mm::comm::FoeError> readFile(uint16_t,
                                                                   const std::string&) override {
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
  int slaves_;
  uint32_t vendorId_;
};

/// A scanned device set. DeviceManager is neither copyable nor movable, so tests hold one in place.
struct Bus {
  DeviceManager dm;

  explicit Bus(uint32_t vendorId = kSynapticonVendorId, int slaves = 2) {
    EXPECT_TRUE(dm.init(std::make_unique<VendorFakeDriver>(slaves, vendorId)).has_value());
    EXPECT_TRUE(dm.scan().has_value());
  }
};

// A well-formed raw OS command request. Command 13 (skipped-cycles counter) is the harmless one: it
// reads a counter and never touches the motor.
//
// Parsed from text rather than built with an initializer list, because that is the only faithful
// mirror of the HTTP path — and the difference is load-bearing. nlohmann stores a non-negative
// integer *parsed* from JSON as unsigned, but one constructed from a C++ `int` literal as signed,
// so a hand-built request fails the byte-range check that the identical parsed request passes.
nlohmann::json osCommandRequest() {
  return nlohmann::json::parse(
      R"({"command": [13, 0, 0, 0, 0, 0, 0, 0], "timeoutMs": 50, "pollIntervalMs": 1})");
}

// Waits out a run so the manager is not destroyed mid-body. What the run *did* is irrelevant here —
// these tests are about resolution and error mapping, not about the bus.
void awaitCompletion(const ProcedureManager& manager, uint16_t position, std::string_view name) {
  for (int attempt = 0; attempt < 2000; ++attempt) {
    auto snapshot = manager.snapshot(position, name);
    if (snapshot && snapshot->status != ProcedureStatus::kRunning) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ADD_FAILURE() << "procedure '" << name << "' never finished";
}

TEST(ProcedureCatalogue, DescribesEveryProcedureItHolds) {
  // The descriptor is what a client renders a control from, so an entry missing its text would show
  // up as a blank card rather than a compile error. Every entry must carry the lot.
  ASSERT_FALSE(procedureCatalogue().empty());
  for (const auto& entry : procedureCatalogue()) {
    EXPECT_FALSE(entry.descriptor.name.empty());
    EXPECT_FALSE(entry.descriptor.title.empty());
    EXPECT_FALSE(entry.descriptor.description.empty());
    EXPECT_FALSE(entry.descriptor.steps.empty()) << entry.descriptor.name;
    EXPECT_TRUE(entry.applies) << entry.descriptor.name;
    EXPECT_TRUE(entry.makeBody) << entry.descriptor.name;
  }
}

TEST(ProcedureCatalogue, DescribesEveryParameterWellEnoughToRenderAField) {
  // A client builds its form from these, so a parameter missing the field its type needs renders as
  // a control with nothing to offer — a select with no options, a byte field of unknown length —
  // rather than as anything that would fail a build.
  for (const auto& entry : procedureCatalogue()) {
    for (const auto& parameter : entry.descriptor.parameters) {
      const std::string where = entry.descriptor.name + "." + parameter.name;
      EXPECT_FALSE(parameter.name.empty()) << entry.descriptor.name;
      EXPECT_FALSE(parameter.title.empty()) << where;
      EXPECT_FALSE(parameter.description.empty()) << where;
      switch (parameter.type) {
        case ParameterType::kInteger:
          EXPECT_TRUE(parameter.minValue.has_value()) << where;
          EXPECT_TRUE(parameter.maxValue.has_value()) << where;
          break;
        case ParameterType::kEnum:
          EXPECT_FALSE(parameter.options.empty()) << where;
          break;
        case ParameterType::kByteArray:
          EXPECT_TRUE(parameter.length.has_value()) << where;
          break;
        // These four need nothing beyond the name/title/description every parameter has: a
        // checkbox, a text box, an editable list of text boxes, and a file picker are all
        // renderable from the type alone.
        case ParameterType::kBoolean:
        case ParameterType::kString:
        case ParameterType::kStringArray:
        case ParameterType::kFile:
          break;
      }
    }
  }
}

TEST(ProcedureCatalogue, ParameterNamesAreUniqueWithinAProcedure) {
  // They are the keys of one request object, so a duplicate would make one of them unreachable.
  for (const auto& entry : procedureCatalogue()) {
    std::vector<std::string> names;
    names.reserve(entry.descriptor.parameters.size());
    for (const auto& parameter : entry.descriptor.parameters) {
      names.push_back(parameter.name);
    }
    std::ranges::sort(names);
    EXPECT_EQ(std::ranges::unique(names).begin(), names.end()) << entry.descriptor.name;
  }
}

TEST(ProcedureCatalogue, NamesAreUnique) {
  // Names address the resource and key the retained snapshot, so a duplicate would make one entry
  // unreachable and let two procedures overwrite each other's results.
  std::vector<std::string> names;
  for (const auto& entry : procedureCatalogue()) {
    names.push_back(entry.descriptor.name);
  }
  std::ranges::sort(names);
  EXPECT_EQ(std::ranges::unique(names).begin(), names.end());
}

TEST(ProcedureCatalogue, IsOrderedByName) {
  // The table's order is what clients render: applicableEntries preserves it, so the list endpoint
  // and the Console's sidebar are ordered by the catalogue being ordered and nothing else. Pinned
  // here because the sort is one line at the end of buildCatalogue, and dropping it would leave a
  // list in authoring order — which every other test would still pass.
  const auto& entries = procedureCatalogue();
  EXPECT_TRUE(std::ranges::is_sorted(entries, {}, [](const auto& entry) {
    return std::string_view(entry.descriptor.name);
  })) << "the catalogue must be sorted by descriptor.name";
}

TEST(ListProcedures, ReportsAnIdleSnapshotForAProcedureNeverRun) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  auto listings = listProcedures(bus.dm, manager, 1);
  ASSERT_TRUE(listings) << listings.error();
  ASSERT_FALSE(listings->empty());

  // The point of pairing descriptor with snapshot: never-run is a well-formed state, not an
  // absence, so a client renders one shape whether or not the procedure has ever been run.
  const auto& listing = listings->front();
  EXPECT_EQ(listing.snapshot.status, ProcedureStatus::kIdle);
  EXPECT_EQ(listing.snapshot.runCount, 0u);
  EXPECT_FALSE(listing.snapshot.startedAt.has_value());
  EXPECT_FALSE(listing.snapshot.finishedAt.has_value());
  // Seeded from the descriptor's template, so the steps are known before the first run.
  ASSERT_EQ(listing.snapshot.steps.size(), listing.descriptor.steps.size());
  EXPECT_EQ(listing.snapshot.steps.front().id, listing.descriptor.steps.front().id);
}

TEST(ListProcedures, OffersNothingOnAnotherVendorsDevice) {
  // Applicability is per device: a vendor's procedures must not be offered on a third-party slave,
  // where they could only ever fail.
  Bus bus(0x0000'0002);
  ProcedureManager manager(bus.dm);

  auto listings = listProcedures(bus.dm, manager, 1);
  ASSERT_TRUE(listings) << listings.error();
  EXPECT_TRUE(listings->empty());
}

TEST(ListProcedures, RejectsAnUnknownDevice) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  auto listings = listProcedures(bus.dm, manager, 99);
  ASSERT_FALSE(listings.has_value());
  EXPECT_EQ(listings.error().kind, ProcedureError::Kind::kUnknownDevice);
}

TEST(ProcedureSnapshot, ReportsIdleBeforeTheFirstRun) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  auto snapshot = procedureSnapshot(bus.dm, manager, 1, kOsCommand);
  ASSERT_TRUE(snapshot) << snapshot.error();
  EXPECT_EQ(snapshot->status, ProcedureStatus::kIdle);
  EXPECT_EQ(snapshot->runCount, 0u);
}

TEST(ProcedureSnapshot, RejectsAnUnknownProcedureName) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  auto snapshot = procedureSnapshot(bus.dm, manager, 1, "no-such-procedure");
  ASSERT_FALSE(snapshot.has_value());
  EXPECT_EQ(snapshot.error().kind, ProcedureError::Kind::kUnknownProcedure);
}

TEST(ProcedureSnapshot, RejectsAProcedureTheDeviceDoesNotSupport) {
  // A name the catalogue holds but this device does not have is the same answer as a name that does
  // not exist: nothing is addressable there.
  Bus bus(0x0000'0002);
  ProcedureManager manager(bus.dm);

  auto snapshot = procedureSnapshot(bus.dm, manager, 1, kOsCommand);
  ASSERT_FALSE(snapshot.has_value());
  EXPECT_EQ(snapshot.error().kind, ProcedureError::Kind::kUnknownProcedure);
}

TEST(StartProcedure, ReturnsTheInitialSnapshotWithABumpedRunCount) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  auto snapshot = startProcedure(bus.dm, manager, 1, kOsCommand, osCommandRequest());
  ASSERT_TRUE(snapshot) << snapshot.error();
  EXPECT_EQ(snapshot->runCount, 1u);
  EXPECT_TRUE(snapshot->startedAt.has_value());
  awaitCompletion(manager, 1, kOsCommand);
}

TEST(StartProcedure, RejectsAnInvalidRequestBeforeAnythingRuns) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  // Validation belongs to the entry, and its failure is a bad request rather than a bad address —
  // the distinction the handler turns into 400 instead of 404.
  auto snapshot = startProcedure(bus.dm, manager, 1, kOsCommand, nlohmann::json::object());
  ASSERT_FALSE(snapshot.has_value());
  EXPECT_EQ(snapshot.error().kind, ProcedureError::Kind::kInvalidRequest);
  // Nothing was started, so no run is retained and runCount stays where it was.
  EXPECT_FALSE(manager.snapshot(1, kOsCommand).has_value());
}

TEST(StartProcedure, RejectsAnUnknownProcedureName) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  auto snapshot = startProcedure(bus.dm, manager, 1, "no-such-procedure", nlohmann::json::object());
  ASSERT_FALSE(snapshot.has_value());
  EXPECT_EQ(snapshot.error().kind, ProcedureError::Kind::kUnknownProcedure);
}

TEST(StartProcedure, RejectsAnUnknownDevice) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  auto snapshot = startProcedure(bus.dm, manager, 99, kOsCommand, osCommandRequest());
  ASSERT_FALSE(snapshot.has_value());
  EXPECT_EQ(snapshot.error().kind, ProcedureError::Kind::kUnknownDevice);
}

TEST(CancelProcedure, RejectsWhenNothingIsRunning) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  auto cancelled = cancelProcedure(bus.dm, manager, 1, kOsCommand);
  ASSERT_FALSE(cancelled.has_value());
  EXPECT_EQ(cancelled.error().kind, ProcedureError::Kind::kUnknownProcedure);
}

TEST(CancelProcedure, RejectsAnUnknownProcedureName) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  auto cancelled = cancelProcedure(bus.dm, manager, 1, "no-such-procedure");
  ASSERT_FALSE(cancelled.has_value());
  EXPECT_EQ(cancelled.error().kind, ProcedureError::Kind::kUnknownProcedure);
}

TEST(ProcedureListingJson, CarriesTheDescriptorAndSnapshotSeparately) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  auto listings = listProcedures(bus.dm, manager, 1);
  ASSERT_TRUE(listings) << listings.error();
  ASSERT_FALSE(listings->empty());

  const nlohmann::json j = listings->front();
  ASSERT_TRUE(j.contains("descriptor"));
  ASSERT_TRUE(j.contains("snapshot"));

  const auto& descriptor = j["descriptor"];
  EXPECT_TRUE(descriptor["name"].is_string());
  EXPECT_TRUE(descriptor["caveats"].is_array());
  EXPECT_TRUE(descriptor["movesMotor"].is_boolean());
  EXPECT_TRUE(descriptor["requiresEnabled"].is_boolean());
  // The descriptor's steps are bare ids — live per-step status belongs to the snapshot, where the
  // same ids appear as objects.
  ASSERT_TRUE(descriptor["steps"].is_array());
  EXPECT_TRUE(descriptor["steps"].front().is_string());
  EXPECT_EQ(j["snapshot"]["steps"].front()["id"], descriptor["steps"].front());
  EXPECT_EQ(j["snapshot"]["status"], "idle");
}

}  // namespace
