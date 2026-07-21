#include "node/process_image.h"

#include <gtest/gtest.h>

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
#include <variant>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "node/device.h"
#include "node/device_manager.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::OdEntry;
using mm::comm::PdoLayout;
using mm::comm::SlaveInfo;
using mm::comm::SlaveIo;
using mm::node::buildProcessImage;
using mm::node::Device;
using mm::node::DeviceManager;
using mm::node::DeviceParameterValue;
using mm::node::extractBits;
using mm::node::insertBits;
using mm::node::ProcessDataRing;

// ETG.1020 data type codes.
constexpr uint16_t kU8 = 0x0005;
constexpr uint16_t kU16 = 0x0006;
constexpr uint16_t kI32 = 0x0004;

std::vector<uint8_t> u8le(uint8_t v) { return {v}; }
std::vector<uint8_t> u16le(uint16_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
}
std::vector<uint8_t> u32le(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}
std::vector<uint8_t> pdoEntry(uint16_t index, uint8_t subindex, uint8_t bits) {
  return u32le((static_cast<uint32_t>(index) << 16) | (static_cast<uint32_t>(subindex) << 8) |
               bits);
}

/// Fieldbus fake: serves canned SDO reads (so readFlatPdoMapping works), returns a programmed
/// process-data layout, and on exchange records the outputs it received and copies a canned
/// input image back. Position-agnostic SDO map — every slave reports the same mapping.
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
  SlaveInfo slaveInfo(uint16_t) const override { return {}; }
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
  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    return ods;
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
};

// Programs a 6-byte-per-direction CiA402-style mapping: controlword + target position out,
// statusword + actual position in. Served for every slave position.
void programMapping(FakeBus& bus) {
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

TEST(BuildProcessImage, RebasesEntriesOntoEachSlaveWindow) {
  FakeBus bus;
  programMapping(bus);

  // Two devices, each 6 bytes per direction; device 2's window follows device 1's.
  Device d1(1, bus);
  Device d2(2, bus);
  ASSERT_TRUE(d1.readFlatPdoMapping().has_value());
  ASSERT_TRUE(d2.readFlatPdoMapping().has_value());
  std::vector<Device> devices;
  devices.push_back(std::move(d1));
  devices.push_back(std::move(d2));

  PdoLayout layout;
  layout.outputBytes = 12;
  layout.inputBytes = 12;
  layout.expectedWkc = 6;
  layout.slaves = {SlaveIo{.slavePosition = 1,
                           .outputOffset = 0,
                           .outputBytes = 6,
                           .inputOffset = 0,
                           .inputBytes = 6},
                   SlaveIo{.slavePosition = 2,
                           .outputOffset = 6,
                           .outputBytes = 6,
                           .inputOffset = 6,
                           .inputBytes = 6}};

  auto image = buildProcessImage(layout, devices);
  ASSERT_TRUE(image.has_value());
  EXPECT_EQ(image->outputBytes, 12u);
  EXPECT_EQ(image->inputBytes, 12u);
  EXPECT_EQ(image->expectedWkc, 6);

  ASSERT_EQ(image->outputs.size(), 4u);
  // Device 1 outputs at the start of the output image.
  EXPECT_EQ(image->outputs[0].index, 0x6040);
  EXPECT_EQ(image->outputs[0].bitOffset, 0u);
  EXPECT_EQ(image->outputs[1].index, 0x607A);
  EXPECT_EQ(image->outputs[1].bitOffset, 16u);
  // Device 2 outputs rebased past device 1's 6-byte window (48 bits).
  EXPECT_EQ(image->outputs[2].slavePosition, 2);
  EXPECT_EQ(image->outputs[2].index, 0x6040);
  EXPECT_EQ(image->outputs[2].bitOffset, 48u);
  EXPECT_EQ(image->outputs[3].index, 0x607A);
  EXPECT_EQ(image->outputs[3].bitOffset, 64u);

  ASSERT_EQ(image->inputs.size(), 4u);
  EXPECT_EQ(image->inputs[2].slavePosition, 2);
  EXPECT_EQ(image->inputs[2].index, 0x6041);
  EXPECT_EQ(image->inputs[2].bitOffset, 48u);
}

TEST(BuildProcessImage, RejectsMappingWiderThanItsWindow) {
  FakeBus bus;
  programMapping(bus);  // 48 output bits
  Device d(1, bus);
  ASSERT_TRUE(d.readFlatPdoMapping().has_value());
  std::vector<Device> devices;
  devices.push_back(std::move(d));

  PdoLayout layout;
  layout.outputBytes = 4;  // only 32 bits — too small for the 48-bit mapping
  layout.inputBytes = 6;
  layout.slaves = {SlaveIo{
      .slavePosition = 1, .outputOffset = 0, .outputBytes = 4, .inputOffset = 0, .inputBytes = 6}};

  auto image = buildProcessImage(layout, devices);
  EXPECT_FALSE(image.has_value());
}

TEST(BuildProcessImage, RejectsImageLargerThanCapacity) {
  // A driver layout whose combined image exceeds the IOmap capacity must be rejected at the shared
  // chokepoint — not left to produce out-of-bounds spans in exchangeProcessData. The cap is on the
  // combined image ([all outputs | all inputs] share one IOmap), not each direction. The size
  // guard runs before any per-device work, so an empty device set is enough to exercise it.
  std::vector<Device> devices;
  PdoLayout layout;

  layout.outputBytes = mm::comm::kMaxProcessImageBytes + 1;
  layout.inputBytes = 0;
  EXPECT_FALSE(buildProcessImage(layout, devices).has_value());

  layout.outputBytes = 0;
  layout.inputBytes = mm::comm::kMaxProcessImageBytes + 1;
  EXPECT_FALSE(buildProcessImage(layout, devices).has_value());

  // Directions that each fit but together exceed capacity are rejected — the cap is on the sum.
  layout.outputBytes = mm::comm::kMaxProcessImageBytes;
  layout.inputBytes = mm::comm::kMaxProcessImageBytes;
  EXPECT_FALSE(buildProcessImage(layout, devices).has_value());

  // A combined image exactly at capacity is accepted (no devices/mappings here, so an empty image).
  layout.outputBytes = mm::comm::kMaxProcessImageBytes / 2;
  layout.inputBytes = mm::comm::kMaxProcessImageBytes / 2;
  EXPECT_TRUE(buildProcessImage(layout, devices).has_value());
}

TEST(DeviceManagerProcessData, ConfigurePublishesAndExchangePublishesInputs) {
  auto bus = std::make_unique<FakeBus>();
  programMapping(*bus);
  bus->slaves = 1;
  bus->layout.outputBytes = 6;
  bus->layout.inputBytes = 6;
  bus->layout.expectedWkc = 3;
  bus->layout.slaves = {SlaveIo{
      .slavePosition = 1, .outputOffset = 0, .outputBytes = 6, .inputOffset = 0, .inputBytes = 6}};
  // Statusword 0x0237, actual position 0x11223344 — little-endian in the input image.
  bus->cannedInputs = {0x37, 0x02, 0x44, 0x33, 0x22, 0x11};

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  EXPECT_FALSE(dm.processDataConfigured());
  ASSERT_TRUE(dm.configureProcessData().has_value());
  EXPECT_TRUE(dm.processDataConfigured());

  // Before any exchange nothing is recorded.
  EXPECT_EQ(dm.recorderHead(), 0u);

  dm.exchangeProcessData();

  // One cycle recorded; its raw input image is the newest record.
  ASSERT_EQ(dm.recorderHead(), 1u);
  ProcessDataRing::Record rec;
  ASSERT_TRUE(dm.readRecord(0, rec));
  ASSERT_EQ(rec.inputs.size(), 6u);
  EXPECT_EQ(rec.inputs[0], 0x37);
  EXPECT_EQ(rec.inputs[1], 0x02);
  EXPECT_EQ(rec.inputs[2], 0x44);
  EXPECT_EQ(rec.inputs[5], 0x11);

  // After reset, exchange is gated off again.
  dm.reset();
  EXPECT_FALSE(dm.processDataConfigured());
  dm.exchangeProcessData();  // no-op, must not crash
}

// --- bit copy helpers --------------------------------------------------------

TEST(ProcessImageBits, ByteAlignedRoundTrip) {
  std::vector<uint8_t> image(6, 0);
  const std::vector<uint8_t> value = {0x44, 0x33, 0x22, 0x11};  // 32-bit @ bit offset 16
  insertBits(image, 16, 32, value);
  EXPECT_EQ(image, (std::vector<uint8_t>{0x00, 0x00, 0x44, 0x33, 0x22, 0x11}));
  EXPECT_EQ(extractBits(image, 16, 32), value);
}

TEST(ProcessImageBits, SubByteRoundTrip) {
  std::vector<uint8_t> image(2, 0);
  // Place the 3-bit value 0b101 at bit offset 5 (straddles the byte boundary).
  insertBits(image, 5, 3, std::vector<uint8_t>{0b101});
  EXPECT_EQ(extractBits(image, 5, 3), (std::vector<uint8_t>{0b101}));
  // A neighbouring field is untouched: 4 bits 0b1111 at offset 0.
  insertBits(image, 0, 4, std::vector<uint8_t>{0b1111});
  EXPECT_EQ(extractBits(image, 0, 4), (std::vector<uint8_t>{0b1111}));
  EXPECT_EQ(extractBits(image, 5, 3), (std::vector<uint8_t>{0b101}));
}

// Builds a single-axis bus with the CiA402 mapping, object dictionary, and layout wired up.
std::unique_ptr<FakeBus> makeCia402Bus() {
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

// Builds a bus with two 4-bit outputs packed into a single output byte: 0x2001 in the low nibble
// (bits 0..3), 0x2002 in the high nibble (bits 4..7). No inputs. Used to prove that two sub-byte
// objects sharing a byte, each written into its own lock-free slot, are composed by the single RT
// thread without clobbering each other.
std::unique_ptr<FakeBus> makeSubByteBus() {
  auto bus = std::make_unique<FakeBus>();
  bus->program(0x1C12, 0x00, u8le(1));
  bus->program(0x1C12, 0x01, u16le(0x1600));
  bus->program(0x1600, 0x00, u8le(2));
  bus->program(0x1600, 0x01, pdoEntry(0x2001, 0x00, 4));  // low nibble @0
  bus->program(0x1600, 0x02, pdoEntry(0x2002, 0x00, 4));  // high nibble @4
  bus->programOd(0x2001, 0x00, kU8);
  bus->programOd(0x2002, 0x00, kU8);
  bus->slaves = 1;
  bus->layout.outputBytes = 1;
  bus->layout.inputBytes = 0;
  bus->layout.expectedWkc = 2;
  bus->layout.slaves = {SlaveIo{
      .slavePosition = 1, .outputOffset = 0, .outputBytes = 1, .inputOffset = 0, .inputBytes = 0}};
  bus->state = static_cast<uint16_t>(EtherCatState::Op);
  return bus;
}

TEST(DeviceManagerProcessData, WriteStagesOutputAndReadPullsInput) {
  auto bus = makeCia402Bus();
  // Statusword 0x0237, actual position 0x11223344 — little-endian in the input image.
  bus->cannedInputs = {0x37, 0x02, 0x44, 0x33, 0x22, 0x11};
  bus->wkc = 3;  // OP device (outputs + inputs) fully contributing — a healthy bus
  FakeBus* busPtr = bus.get();

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, false).has_value());
  ASSERT_TRUE(dm.configureProcessData().has_value());

  // Writing a PDO output object stages it (no SDO download recorded).
  ASSERT_TRUE(
      dm.writeDeviceParameter(1, 0x6040, 0x00, DeviceParameterValue{uint16_t{0x000F}}).has_value());
  dm.exchangeProcessData();
  ASSERT_GE(busPtr->lastOutputs.size(), 2u);
  EXPECT_EQ(busPtr->lastOutputs[0], 0x0F);  // controlword low byte
  EXPECT_EQ(busPtr->lastOutputs[1], 0x00);

  // Reading a PDO input object pulls the live value from the input snapshot.
  auto status = dm.readDeviceParameter(1, 0x6041, 0x00);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(std::get<uint16_t>(*status), 0x0237);
  // The cached parameter now reflects the live value too.
  EXPECT_EQ(std::get<uint16_t>(dm.findDevice(1)->parameter(0x6041, 0x00)->value), 0x0237);
}

TEST(DeviceManagerProcessData, IndependentOutputSlotsComposeIntoOneImage) {
  auto bus = makeCia402Bus();
  bus->wkc = 3;
  FakeBus* busPtr = bus.get();

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, false).has_value());
  ASSERT_TRUE(dm.configureProcessData().has_value());

  // Two distinct output objects, each staged into its own lock-free slot — no shared buffer, no
  // lock. The RT loop is the sole composer that assembles them into the packed wire image.
  ASSERT_TRUE(
      dm.writeDeviceParameter(1, 0x6040, 0x00, DeviceParameterValue{uint16_t{0xABCD}}).has_value());
  ASSERT_TRUE(dm.writeDeviceParameter(1, 0x607A, 0x00, DeviceParameterValue{int32_t{0x11223344}})
                  .has_value());
  dm.exchangeProcessData();

  ASSERT_EQ(busPtr->lastOutputs.size(), 6u);
  EXPECT_EQ(busPtr->lastOutputs[0], 0xCD);  // controlword (0x6040) little-endian @ byte 0
  EXPECT_EQ(busPtr->lastOutputs[1], 0xAB);
  EXPECT_EQ(busPtr->lastOutputs[2], 0x44);  // target position (0x607A) little-endian @ byte 2
  EXPECT_EQ(busPtr->lastOutputs[3], 0x33);
  EXPECT_EQ(busPtr->lastOutputs[4], 0x22);
  EXPECT_EQ(busPtr->lastOutputs[5], 0x11);
}

TEST(DeviceManagerProcessData, SubByteOutputsSharingAByteComposeWithoutClobber) {
  auto bus = makeSubByteBus();
  FakeBus* busPtr = bus.get();

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, false).has_value());
  ASSERT_TRUE(dm.configureProcessData().has_value());

  // Two objects share byte 0; each is written independently into its own slot.
  ASSERT_TRUE(
      dm.writeDeviceParameter(1, 0x2001, 0x00, DeviceParameterValue{uint8_t{0x0A}}).has_value());
  ASSERT_TRUE(
      dm.writeDeviceParameter(1, 0x2002, 0x00, DeviceParameterValue{uint8_t{0x05}}).has_value());
  dm.exchangeProcessData();

  // The single RT composer packed both nibbles into byte 0: (0x05 << 4) | 0x0A = 0x5A. Neither
  // write clobbered the other — exactly the race the old shared-buffer RMW needed the mutex for.
  ASSERT_EQ(busPtr->lastOutputs.size(), 1u);
  EXPECT_EQ(busPtr->lastOutputs[0], 0x5A);
}

TEST(DeviceManagerProcessData, OutputReadBackServesStagedSlotNotSdoAndIgnoresHealth) {
  auto bus = makeCia402Bus();
  bus->program(0x6040, 0x00, u16le(0x1111));  // distinct authoritative SDO value for the output
  bus->wkc = 1;                               // below expected 3 → unhealthy bus
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, false).has_value());
  ASSERT_TRUE(dm.configureProcessData().has_value());
  dm.exchangeProcessData();
  ASSERT_FALSE(dm.processDataHealthy());

  // Stage a controlword distinct from the SDO value.
  ASSERT_TRUE(
      dm.writeDeviceParameter(1, 0x6040, 0x00, DeviceParameterValue{uint16_t{0x000F}}).has_value());

  // An output read returns the staged slot (0x000F) — our own setpoint, always valid and lock-free
  // — never the SDO value (0x1111). Unlike an input, it is NOT gated on bus health: a momentarily
  // short working counter must not push an RT caller onto a blocking SDO upload for its own output.
  auto v = dm.readDeviceParameter(1, 0x6040, 0x00);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(std::get<uint16_t>(*v), 0x000F);
}

TEST(DeviceManagerProcessData, OutputsInitialisedBeforeOpAreSentOnFirstCycle) {
  auto bus = makeCia402Bus();
  FakeBus* busPtr = bus.get();

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, false).has_value());

  // Set the target position BEFORE configuring process data (i.e. before OP). The image is
  // not yet published, so this lands in the cached parameter.
  ASSERT_TRUE(dm.writeDeviceParameter(1, 0x607A, 0x00, DeviceParameterValue{int32_t{0x00405060}})
                  .has_value());

  // configureProcessData seeds the output staging from the cached values.
  ASSERT_TRUE(dm.configureProcessData().has_value());
  dm.exchangeProcessData();

  // Target position occupies bytes 2..5 of the output image (after the 16-bit controlword).
  ASSERT_EQ(busPtr->lastOutputs.size(), 6u);
  EXPECT_EQ(busPtr->lastOutputs[2], 0x60);
  EXPECT_EQ(busPtr->lastOutputs[3], 0x50);
  EXPECT_EQ(busPtr->lastOutputs[4], 0x40);
  EXPECT_EQ(busPtr->lastOutputs[5], 0x00);
}

TEST(DeviceManagerProcessData, MappingConfiguredAndTornDownReactingToState) {
  const auto kTimeout = std::chrono::milliseconds(10);
  auto bus = makeCia402Bus();
  bus->cannedInputs = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  FakeBus* busPtr = bus.get();

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, false).has_value());

  EXPECT_FALSE(dm.processDataConfigured());

  // Entering SAFE-OP auto-configures and publishes the mapping.
  ASSERT_TRUE(dm.transitionToState({}, EtherCatState::SafeOp, kTimeout).has_value());
  EXPECT_TRUE(dm.processDataConfigured());
  dm.exchangeProcessData();
  ProcessDataRing::Record rec;
  ASSERT_TRUE(dm.readRecord(dm.recorderHead() - 1, rec));
  EXPECT_EQ(rec.inputs.size(), 6u);

  // SAFE-OP -> OP keeps the existing mapping (no re-map).
  ASSERT_TRUE(dm.transitionToState({}, EtherCatState::Op, kTimeout).has_value());
  EXPECT_TRUE(dm.processDataConfigured());

  // Leaving the exchanging states (e.g. down to PRE-OP for a firmware download) tears it down.
  ASSERT_TRUE(dm.transitionToState({}, EtherCatState::PreOp, kTimeout).has_value());
  EXPECT_FALSE(dm.processDataConfigured());

  // Returning to SAFE-OP re-maps the bus, and exchange works again — mapping continues to
  // function across a down/up cycle.
  busPtr->lastOutputs.clear();
  ASSERT_TRUE(dm.transitionToState({}, EtherCatState::SafeOp, kTimeout).has_value());
  EXPECT_TRUE(dm.processDataConfigured());
  dm.exchangeProcessData();
  EXPECT_EQ(busPtr->lastOutputs.size(), 6u);
}

TEST(DeviceManagerProcessData, SubsetDownKeepsOthersExchangingAndRejoinRemaps) {
  // A subset of the bus can be taken down (firmware download / manual PDO re-map) while the rest
  // keep exchanging; bringing it back re-maps the whole bus, since its PDO layout may have changed.
  const auto kTimeout = std::chrono::milliseconds(10);
  auto bus = std::make_unique<FakeBus>();
  programMapping(*bus);  // position-agnostic mapping for both devices
  bus->slaves = 2;
  bus->layout.outputBytes = 12;
  bus->layout.inputBytes = 12;
  bus->layout.expectedWkc = 6;
  bus->layout.slaves = {SlaveIo{.slavePosition = 1,
                                .outputOffset = 0,
                                .outputBytes = 6,
                                .inputOffset = 0,
                                .inputBytes = 6},
                        SlaveIo{.slavePosition = 2,
                                .outputOffset = 6,
                                .outputBytes = 6,
                                .inputOffset = 6,
                                .inputBytes = 6}};
  bus->slaveStates[1] = static_cast<uint16_t>(EtherCatState::Op);
  bus->slaveStates[2] = static_cast<uint16_t>(EtherCatState::Op);
  bus->wkc = 6;
  FakeBus* busPtr = bus.get();

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.configureProcessData().has_value());
  EXPECT_TRUE(dm.processDataConfigured());
  EXPECT_EQ(dm.processImageInfo().generations, 1u);

  // Take device 2 down to PRE-OP. Device 1 stays in OP, so the whole-bus image is kept — no
  // teardown, no re-map — and the staying device keeps exchanging. (Before the fix this nulled
  // the single whole-bus image and silently stopped exchange for device 1 too.)
  ASSERT_TRUE(dm.transitionToState({2}, EtherCatState::PreOp, kTimeout).has_value());
  EXPECT_TRUE(dm.processDataConfigured());
  EXPECT_EQ(dm.processImageInfo().generations, 1u);

  // Model device 2 now sitting in PRE-OP, then climb it back to SAFE-OP (PRE-OP -> OP would be an
  // illegal AL jump): rejoining from a non-exchange state re-maps the whole bus (a new image
  // generation), since its PDO layout may have changed.
  busPtr->slaveStates[2] = static_cast<uint16_t>(EtherCatState::PreOp);
  ASSERT_TRUE(dm.transitionToState({2}, EtherCatState::SafeOp, kTimeout).has_value());
  EXPECT_TRUE(dm.processDataConfigured());
  EXPECT_EQ(dm.processImageInfo().generations, 2u);
}

TEST(DeviceManagerProcessData, RejectsIllegalAlStateTransitions) {
  // Motion Master enforces the EtherCAT AL state machine before touching the bus. The critical
  // case is BOOT -> SAFE-OP/OP: entering an exchange state re-maps by reading PDO mappings over the
  // CoE mailbox, which a device in BOOT cannot serve — that read segfaults inside SOEM. So an
  // illegal jump must be rejected up front, never reaching the mapper.
  const auto kNow = std::chrono::milliseconds(0);
  auto bus = makeCia402Bus();
  FakeBus* busPtr = bus.get();
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  // BOOT reaches only INIT.
  busPtr->state = static_cast<uint16_t>(EtherCatState::Boot);
  EXPECT_FALSE(dm.transitionToState({}, EtherCatState::PreOp, kNow).has_value());
  EXPECT_FALSE(dm.transitionToState({}, EtherCatState::SafeOp, kNow).has_value());
  EXPECT_FALSE(dm.transitionToState({}, EtherCatState::Op, kNow).has_value());
  EXPECT_TRUE(dm.transitionToState({}, EtherCatState::Init, kNow).has_value());

  // INIT climbs only to PRE-OP (or enters BOOT); skipping to SAFE-OP/OP is rejected.
  busPtr->state = static_cast<uint16_t>(EtherCatState::Init);
  EXPECT_FALSE(dm.transitionToState({}, EtherCatState::SafeOp, kNow).has_value());
  EXPECT_FALSE(dm.transitionToState({}, EtherCatState::Op, kNow).has_value());
  EXPECT_TRUE(dm.transitionToState({}, EtherCatState::PreOp, kNow).has_value());
  EXPECT_TRUE(dm.transitionToState({}, EtherCatState::Boot, kNow).has_value());

  // PRE-OP climbs only one step (to SAFE-OP); OP is two steps and BOOT is reachable only from INIT.
  busPtr->state = static_cast<uint16_t>(EtherCatState::PreOp);
  EXPECT_FALSE(dm.transitionToState({}, EtherCatState::Op, kNow).has_value());
  EXPECT_FALSE(dm.transitionToState({}, EtherCatState::Boot, kNow).has_value());
  EXPECT_TRUE(dm.transitionToState({}, EtherCatState::Init, kNow).has_value());

  // Drops may skip levels: OP -> INIT is allowed.
  busPtr->state = static_cast<uint16_t>(EtherCatState::Op);
  EXPECT_TRUE(dm.transitionToState({}, EtherCatState::Init, kNow).has_value());
}

TEST(DeviceManagerProcessData, MixedStatesRoutePerDeviceBetweenPdoAndSdo) {
  // Two devices share one whole-bus mapping, but device 1 stays in PRE-OP while device 2 is
  // in OP. Device 2's params come from the process image; device 1's must come over SDO, not
  // from its stale (never-filled) region of the snapshot.
  auto bus = std::make_unique<FakeBus>();
  programMapping(*bus);                // position-agnostic PDO assignment for both
  bus->programOd(0x6040, 0x00, kU16);  // controlword
  bus->programOd(0x607A, 0x00, kI32);  // target position
  bus->programOd(0x6041, 0x00, kU16);  // statusword
  bus->programOd(0x6064, 0x00, kI32);  // actual position
  bus->slaves = 2;
  bus->layout.outputBytes = 12;
  bus->layout.inputBytes = 12;
  bus->layout.expectedWkc = 6;
  bus->layout.slaves = {SlaveIo{.slavePosition = 1,
                                .outputOffset = 0,
                                .outputBytes = 6,
                                .inputOffset = 0,
                                .inputBytes = 6},
                        SlaveIo{.slavePosition = 2,
                                .outputOffset = 6,
                                .outputBytes = 6,
                                .inputOffset = 6,
                                .inputBytes = 6}};
  // Snapshot: device 1's statusword region (bytes 0..1) holds stale data; device 2's
  // statusword (bytes 6..7) holds the live value 0x1234.
  bus->cannedInputs = {0x99, 0x99, 0, 0, 0, 0, 0x34, 0x12, 0, 0, 0, 0};
  bus->program(0x6041, 0x00, u16le(0xABCD));  // distinct SDO value for device 1's statusword
  bus->slaveStates[1] = static_cast<uint16_t>(EtherCatState::PreOp);
  bus->slaveStates[2] = static_cast<uint16_t>(EtherCatState::Op);
  bus->wkc = 3;  // only device 2 (OP) contributes, so 3 == expected: a healthy bus

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, false).has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(2, false).has_value());
  ASSERT_TRUE(dm.configureProcessData().has_value());  // whole-bus image, both devices
  dm.exchangeProcessData();

  // Device 2 (OP) reads its statusword from the process image.
  auto s2 = dm.readDeviceParameter(2, 0x6041, 0x00);
  ASSERT_TRUE(s2.has_value());
  EXPECT_EQ(std::get<uint16_t>(*s2), 0x1234);

  // Device 1 (PRE-OP) reads over SDO — the live mailbox value, not its stale 0x9999 region.
  auto s1 = dm.readDeviceParameter(1, 0x6041, 0x00);
  ASSERT_TRUE(s1.has_value());
  EXPECT_EQ(std::get<uint16_t>(*s1), 0xABCD);

  // Only device 2 (OP, outputs + inputs) contributes to the expected WKC; device 1 (PRE-OP)
  // contributes nothing, so the partially-operational bus expects 3, not 6.
  EXPECT_EQ(dm.expectedWorkingCounter(), 3);
}

TEST(DeviceManagerProcessData, WorkingCounterHealthReflectsParticipation) {
  auto bus = makeCia402Bus();  // one device in OP, with outputs and inputs
  FakeBus* busPtr = bus.get();
  bus->wkc = 3;  // OP device with outputs (2) + inputs (1)

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, false).has_value());
  ASSERT_TRUE(dm.configureProcessData().has_value());

  EXPECT_EQ(dm.expectedWorkingCounter(), 3);

  dm.exchangeProcessData();
  EXPECT_EQ(dm.lastWorkingCounter(), 3);
  EXPECT_TRUE(dm.processDataHealthy());

  // A device dropping out shows up as a working counter below the expectation.
  busPtr->wkc = 1;
  dm.exchangeProcessData();
  EXPECT_EQ(dm.lastWorkingCounter(), 1);
  EXPECT_FALSE(dm.processDataHealthy());
}

TEST(DeviceManagerProcessData, UnhealthyWorkingCounterReadsFallBackToSdo) {
  // Regression: on a lost or partial frame the driver leaves the prior cycle's bytes in the
  // IOmap, so the input snapshot is stale. A read must not serve that stale value as live — it
  // falls back to the authoritative SDO upload, which reflects the device's real state.
  auto bus = makeCia402Bus();                    // single device in OP, expected WKC 3
  bus->cannedInputs = {0x37, 0x02, 0, 0, 0, 0};  // statusword 0x0237 sits in the input image
  bus->program(0x6041, 0x00, u16le(0xABCD));     // distinct authoritative SDO value
  bus->wkc = 1;                                  // below expected 3 → a slave dropped out
  FakeBus* busPtr = bus.get();

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, false).has_value());
  ASSERT_TRUE(dm.configureProcessData().has_value());
  dm.exchangeProcessData();
  ASSERT_FALSE(dm.processDataHealthy());

  // Unhealthy bus: the read ignores the stale 0x0237 snapshot and uses the SDO value.
  auto stale = dm.readDeviceParameter(1, 0x6041, 0x00);
  ASSERT_TRUE(stale.has_value());
  EXPECT_EQ(std::get<uint16_t>(*stale), 0xABCD);

  // Once the working counter recovers, the read serves the live snapshot value again.
  busPtr->wkc = 3;
  dm.exchangeProcessData();
  ASSERT_TRUE(dm.processDataHealthy());
  auto live = dm.readDeviceParameter(1, 0x6041, 0x00);
  ASSERT_TRUE(live.has_value());
  EXPECT_EQ(std::get<uint16_t>(*live), 0x0237);
}

// --- Step 5a: off-thread sampling read surface ------------------------------

TEST(DeviceManagerSampling, PdoSampleSpecResolvesInputAndOutputObjects) {
  auto bus = makeCia402Bus();
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, false).has_value());
  ASSERT_TRUE(dm.configureProcessData().has_value());

  // Actual position (TxPDO input): 32 bits at offset 16 in the input image, INTEGER32.
  auto in = dm.pdoSampleSpec(1, 0x6064, 0x00);
  ASSERT_TRUE(in.has_value());
  EXPECT_FALSE(in->isOutput);
  EXPECT_EQ(in->bitOffset, 16u);
  EXPECT_EQ(in->bitLength, 32u);
  EXPECT_EQ(in->dataType, kI32);

  // Controlword (RxPDO output): 16 bits at offset 0 in the output image, UNSIGNED16.
  auto out = dm.pdoSampleSpec(1, 0x6040, 0x00);
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(out->isOutput);
  EXPECT_EQ(out->bitOffset, 0u);
  EXPECT_EQ(out->bitLength, 16u);
  EXPECT_EQ(out->dataType, kU16);
}

TEST(DeviceManagerSampling, PdoSampleSpecRejectsUnmappedAndUnconfigured) {
  auto bus = makeCia402Bus();
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, false).has_value());

  // No image published yet → nullopt.
  EXPECT_FALSE(dm.pdoSampleSpec(1, 0x6064, 0x00).has_value());

  ASSERT_TRUE(dm.configureProcessData().has_value());
  // An object that is not PDO-mapped → nullopt.
  EXPECT_FALSE(dm.pdoSampleSpec(1, 0x6065, 0x00).has_value());
  // Unknown device → nullopt.
  EXPECT_FALSE(dm.pdoSampleSpec(99, 0x6064, 0x00).has_value());
}

TEST(DeviceManagerSampling, ProcessImageGenerationBumpsOnEachMap) {
  auto bus = makeCia402Bus();
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, false).has_value());

  const uint64_t before = dm.processImageGeneration();
  ASSERT_TRUE(dm.configureProcessData().has_value());
  const uint64_t afterFirst = dm.processImageGeneration();
  EXPECT_GT(afterFirst, before);
  ASSERT_TRUE(dm.configureProcessData().has_value());  // re-map
  EXPECT_GT(dm.processImageGeneration(), afterFirst);
}

TEST(DeviceManagerSampling, ValueReturnsCachedValueWithoutBus) {
  auto bus = makeCia402Bus();
  FakeBus* busPtr = bus.get();
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, false).has_value());

  // Seed the cache via an offline write (no bus), then read it back through value().
  busPtr->state = static_cast<uint16_t>(EtherCatState::Init);  // offline → cache-only write
  ASSERT_TRUE(
      dm.writeDeviceParameter(1, 0x6041, 0x00, DeviceParameterValue{uint16_t{0xBEEF}}).has_value());

  auto v = dm.value(1, 0x6041, 0x00);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(std::get<uint16_t>(*v), 0xBEEF);

  EXPECT_FALSE(dm.value(1, 0x1234, 0x00).has_value());   // unknown parameter
  EXPECT_FALSE(dm.value(99, 0x6041, 0x00).has_value());  // unknown device
}

}  // namespace
