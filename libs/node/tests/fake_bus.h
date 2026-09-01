#pragma once

// The fieldbus driver test double that the node tests share, and the one-drive bus they build on
// it. Extracted from process_image_test.cc so that more than one test file can drive a whole
// DeviceManager without repeating eighty lines of overrides.
//
// **Thread-safety, because the concurrency tests depend on it.** Everything the double returns is
// programmed before the threads start and only read afterwards. The two mutable members —
// `exchangeCalls` and `lastOutputs` — are written by `exchangeProcessData` alone, so a test must
// call that from one thread only, exactly as the real RT loop does.

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"

namespace mm::node::testing {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::OdEntry;
using mm::comm::OdRead;
using mm::comm::PdoLayout;
using mm::comm::SlaveInfo;
using mm::comm::SlaveIo;

// ETG.1020 data type codes.
constexpr uint16_t kU8 = 0x0005;
constexpr uint16_t kU16 = 0x0006;
constexpr uint16_t kI32 = 0x0004;

inline std::vector<uint8_t> u8le(uint8_t v) { return {v}; }
inline std::vector<uint8_t> u16le(uint16_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
}
inline std::vector<uint8_t> u32le(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}
inline std::vector<uint8_t> pdoEntry(uint16_t index, uint8_t subindex, uint8_t bits) {
  return u32le((static_cast<uint32_t>(index) << 16) | (static_cast<uint32_t>(subindex) << 8) |
               bits);
}

/// Fieldbus fake: serves canned SDO reads (so readFlatPdoMapping works), returns a programmed
/// process-data layout, and on exchange records the outputs it received and copies a canned input
/// image back. Position-agnostic SDO map — every slave reports the same mapping.
class FakeBus : public FieldbusDriver {
 public:
  std::map<uint32_t, std::vector<uint8_t>> reads;
  std::vector<OdEntry> ods;  // returned by readObjectDictionary (for initializeParameters)
  PdoLayout layout;
  int slaves = 1;
  std::vector<uint8_t> cannedInputs;  // copied back on each exchange
  std::vector<uint8_t> lastOutputs;   // captured from the last exchange
  int exchangeCalls = 0;
  uint16_t state = 0;                        // default AL status returned by slaveState()
  std::map<uint16_t, uint16_t> slaveStates;  // per-position override of state
  int wkc = 0;                               // working counter returned by exchangeProcessData

  static uint32_t key(uint16_t index, uint8_t sub) {
    return (static_cast<uint32_t>(index) << 8) | sub;
  }
  void program(uint16_t index, uint8_t sub, std::vector<uint8_t> bytes) {
    reads[key(index, sub)] = std::move(bytes);
  }
  void programOd(uint16_t index, uint8_t sub, uint16_t dataType) {
    OdEntry e{};
    e.index = index;
    e.subindex = sub;
    e.dataType = dataType;
    ods.push_back(e);
  }

  std::expected<int, std::string> scan() override { return slaves; }
  uint16_t slaveState(uint16_t position) const override {
    auto it = slaveStates.find(position);
    return it != slaveStates.end() ? it->second : state;
  }
  // CoE-capable stand-in: PDO mapping is read over the mailbox, not from SII.
  uint16_t mailboxProtocols(uint16_t) const override {
    return mm::comm::MailboxConfig::kProtocolCoe;
  }
  std::expected<void, std::string> configureProcessData() override { return {}; }
  PdoLayout processDataLayout() override { return layout; }

  // Static configuration and EEPROM, for callers that describe the bus rather than drive it.
  // Both default to empty, which is what the base class returns, so a test that does not set them
  // sees no change.
  std::vector<mm::comm::SlaveConfig> slaveConfigs;
  std::vector<uint8_t> siiImage;
  std::string siiError;    // non-empty makes every SII read fail
  std::string deviceName;  // returned by slaveInfo(), which callers use as the device's name
  std::vector<mm::comm::SlaveConfig> busConfig() const override { return slaveConfigs; }
  std::expected<std::vector<uint8_t>, std::string> readSii(uint16_t) override {
    if (!siiError.empty()) {
      return std::unexpected(siiError);
    }
    return siiImage;
  }

  int exchangeProcessData(std::span<const uint8_t> outputs, std::span<uint8_t> inputs) override {
    ++exchangeCalls;
    lastOutputs.assign(outputs.begin(), outputs.end());
    for (size_t i = 0; i < inputs.size() && i < cannedInputs.size(); ++i) {
      inputs[i] = cannedInputs[i];
    }
    return wkc;
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t index,
                                                           uint8_t sub) override {
    auto it = reads.find(key(index, sub));
    if (it == reads.end()) {
      return std::unexpected("no such object");
    }
    return it->second;
  }

  // --- unused stubs ---------------------------------------------------------
  std::expected<void, std::string> init() override { return {}; }
  SlaveInfo slaveInfo(uint16_t) const override { return SlaveInfo{.name = deviceName}; }
  void stop() override {}
  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& p) override {
    // Mirror slaveState() so the per-position state the test set up is what callers read back —
    // the same source DeviceManager validates AL transitions against.
    std::vector<SlaveStateRaw> out;
    out.reserve(p.size());
    for (uint16_t pos : p) {
      out.push_back(SlaveStateRaw{.alStatus = slaveState(pos), .alStatusCode = 0});
    }
    return out;
  }
  std::expected<void, std::string> writeSdo(uint16_t, uint16_t, uint8_t,
                                            std::span<const uint8_t>) override {
    return {};
  }
  int odReadCalls = 0;       // how many times the enumeration was attempted
  std::string odError;       // non-empty makes every enumeration fail
  int odMissingEntries = 0;  // above zero reports a lossy enumeration, which must not be cached
  std::expected<OdRead, std::string> readObjectDictionary(uint16_t) override {
    ++odReadCalls;
    if (!odError.empty()) {
      return std::unexpected(odError);
    }
    return OdRead{.entries = ods, .missingEntries = odMissingEntries};
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
  // Runs while the transition is "in progress", standing in for the seconds a real one takes. It
  // is the only way a test can observe what the RT loop sees mid-transition, which is where the
  // working-counter expectation has to already be correct.
  std::function<void()> whileTransitioning;

  void transitionToState(const std::vector<uint16_t>&, std::optional<EtherCatState>, EtherCatState,
                         std::chrono::steady_clock::duration, std::chrono::steady_clock::duration,
                         std::function<void()>, std::function<bool()>) override {
    if (whileTransitioning) {
      whileTransitioning();
    }
  }
};

// Programs a 6-byte-per-direction CiA402-style mapping: controlword + target position out,
// statusword + actual position in. Served for every slave position.
inline void programMapping(FakeBus& bus) {
  bus.program(0x1C12, 0x00, u8le(1));
  bus.program(0x1C12, 0x01, u16le(0x1600));
  bus.program(0x1600, 0x00, u8le(2));
  bus.program(0x1600, 0x01, pdoEntry(0x6040, 0x00, 16));  // controlword @0
  bus.program(0x1600, 0x02, pdoEntry(0x607A, 0x00, 32));  // target position @16
  bus.program(0x1C13, 0x00, u8le(1));
  bus.program(0x1C13, 0x01, u16le(0x1A00));
  bus.program(0x1A00, 0x00, u8le(2));
  bus.program(0x1A00, 0x01, pdoEntry(0x6041, 0x00, 16));  // statusword @0
  bus.program(0x1A00, 0x02, pdoEntry(0x6064, 0x00, 32));  // actual position @16
}

inline std::unique_ptr<FakeBus> makeCia402Bus() {
  auto bus = std::make_unique<FakeBus>();
  programMapping(*bus);
  bus->programOd(0x6040, 0x00, kU16);  // controlword
  bus->programOd(0x607A, 0x00, kI32);  // target position
  bus->programOd(0x6041, 0x00, kU16);  // statusword
  bus->programOd(0x6064, 0x00, kI32);  // actual position
  bus->slaves = 1;
  bus->layout.outputBytes = 6;
  bus->layout.inputBytes = 6;
  bus->layout.expectedWkc = 3;
  bus->layout.slaves = {SlaveIo{
      .slavePosition = 1, .outputOffset = 0, .outputBytes = 6, .inputOffset = 0, .inputBytes = 6}};
  // Report OP so the device counts as exchanging and PDO-mapped access uses the buffers.
  bus->state = static_cast<uint16_t>(EtherCatState::Op);
  return bus;
}

}  // namespace mm::node::testing
