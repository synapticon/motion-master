#include "comm/spoe_fieldbus_driver.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "comm/spoe_connection.h"
#include "comm/spoe_process_data.h"

namespace mm::comm::spoe {

namespace {

// `SQI_REPLY_STATUS_*`: the low status byte of a parameter-list reply.
constexpr uint8_t kReplyAck = 0x58;
constexpr uint8_t kReplyBusy = 0x28;
constexpr uint8_t kReplyError = 0x63;

// The packet states of a segmented transfer: the low status byte of a request, the high status
// byte of a reply.
constexpr uint8_t kPacketFirst = 0x80;
constexpr uint8_t kPacketMiddle = 0xC0;
constexpr uint8_t kPacketLast = 0x40;

// The firmware sends seven entries per packet and lists at most 200 objects, so a whole
// dictionary fits far fewer packets than this. The bound only stops a drive that never sends the
// last packet.
constexpr int kMaxParameterListPackets = 2000;

// The SDO status `AppUtil_GetParameter` and `AppUtil_SetParameter` answer in INIT and BOOT.
constexpr uint16_t kSdoNotAllowedInState = 0xFFFF;

uint16_t readU16(std::span<const uint8_t> bytes, std::size_t offset) {
  return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

uint32_t readU32(std::span<const uint8_t> bytes, std::size_t offset) {
  return readU16(bytes, offset) | (static_cast<uint32_t>(readU16(bytes, offset + 2)) << 16);
}

std::vector<uint8_t> address(uint16_t index, uint16_t subindex) {
  return {static_cast<uint8_t>(index), static_cast<uint8_t>(index >> 8),
          static_cast<uint8_t>(subindex), static_cast<uint8_t>(subindex >> 8)};
}

bool isOpenObject(uint16_t objectCode) {
  // ARRAY (0x08) and RECORD (0x09) have entries after subindex 0. VAR (0x07) has none.
  return objectCode == 0x08 || objectCode == 0x09;
}

}  // namespace

std::vector<ParameterEntry> decodeParameterEntries(std::span<const uint8_t> bytes) {
  // index u16 @0, subindex u8 @2, objectDataType u8 @3, dataType u16 @4, objectCode u8 @6,
  // padding @7, bitLength u16 @8, objectAccess u16 @10, value u32 @12, name[50] @16, padding @66.
  std::vector<ParameterEntry> entries;
  for (std::size_t offset = 0; offset + kParameterEntrySize <= bytes.size();
       offset += kParameterEntrySize) {
    const auto raw = bytes.subspan(offset, kParameterEntrySize);
    const uint16_t index = readU16(raw, 0);
    if (index == 0) {
      continue;
    }
    const auto name = raw.subspan(16, 50);
    const auto end = std::find(name.begin(), name.end(), uint8_t{0});
    entries.push_back(ParameterEntry{.entry = OdEntry{.index = index,
                                                      .subindex = raw[2],
                                                      .objectCode = raw[6],
                                                      .dataType = readU16(raw, 4),
                                                      .bitLength = readU16(raw, 8),
                                                      .access = readU16(raw, 10),
                                                      .name = std::string(name.begin(), end),
                                                      .unit = std::nullopt,
                                                      .defaultValue = std::nullopt,
                                                      .minValue = std::nullopt,
                                                      .maxValue = std::nullopt},
                                     .subindexCount = readU32(raw, 12)});
  }
  return entries;
}

std::string sdoStatusName(uint16_t status) {
  switch (status) {
    case 0:
      return "no error";
    case 1:
      return "generic error";
    case 2:
      return "object not found";
    case 3:
      return "read only";
    case 4:
      return "write only";
    case 5:
      return "wrong type";
    case 6:
      return "invalid list";
    case 7:
      return "insufficient buffer";
    case 8:
      return "value information unavailable";
    case 9:
      return "unknown or unsupported";
    case 10:
      return "local transfer failed";
    case 11:
      return "unsupported access";
    case 12:
      return "subindex 0 is not zero";
    case 13:
      return "subindex not found";
    case kSdoNotAllowedInState:
      return "not allowed in INIT or BOOT";
    default:
      return "unknown status";
  }
}

struct SpoeFieldbusDriver::Drive {
  std::string host;
  SpoeConnection connection;
  // The last known state code, or 0 when none is known. Read lock-free by slaveState.
  std::atomic<uint16_t> state{0};
  // Written by scan and read by slaveInfo, both under controlPlaneMutex_.
  SlaveInfo info;

  // Process data. The window is written by configureProcessData while no exchange runs, and is
  // read by the RT thread and the exchange thread after that.
  SlaveIo io;
  SpoeOutputSlot outputs;
  SpoeInputQueue inputs;
  // Touched by the exchange thread only.
  SpoeFrameAssembler assembler;
  // True while the last process-data exchange got an answer. It decides this drive's share of the
  // working counter.
  std::atomic<bool> exchanging{false};
  std::atomic<uint64_t> droppedFrames{0};
  std::jthread exchange;
};

SpoeFieldbusDriver::SpoeFieldbusDriver(SpoeFieldbusDriverConfig config)
    : config_(std::move(config)) {
  drives_.reserve(config_.hosts.size());
  for (const std::string& host : config_.hosts) {
    auto drive = std::make_unique<Drive>();
    drive->host = host;
    drives_.push_back(std::move(drive));
  }
}

// A clean shutdown destroys the driver without stop(), so this releases the drives too.
SpoeFieldbusDriver::~SpoeFieldbusDriver() { releaseDrives(); }

void SpoeFieldbusDriver::stopExchanges() {
  for (const auto& drive : drives_) {
    if (drive->exchange.joinable()) {
      drive->exchange.request_stop();
      drive->exchange.join();
    }
    drive->exchanging.store(false);
  }
}

void SpoeFieldbusDriver::closeAll() {
  stopExchanges();
  for (const auto& drive : drives_) {
    drive->connection.close();
    drive->state.store(0);
  }
}

SpoeFieldbusDriver::Drive* SpoeFieldbusDriver::driveAt(uint16_t position) const {
  if (position == 0 || position > drives_.size()) {
    return nullptr;
  }
  return drives_[position - 1].get();
}

std::expected<Frame, std::string> SpoeFieldbusDriver::request(Drive& drive, MessageType type,
                                                              std::span<const uint8_t> data,
                                                              uint16_t status) {
  auto reply = drive.connection.request(type, status, data, config_.requestTimeout);
  if (!reply) {
    if (reply.error().kind == ConnectionError::Kind::kClosed) {
      drive.state.store(0);
    }
    return std::unexpected(reply.error().message);
  }
  return std::move(*reply);
}

std::expected<void, std::string> SpoeFieldbusDriver::init() {
  if (drives_.empty()) {
    return std::unexpected("SPoE needs at least one IP address in fieldbus.ipAddresses");
  }
  spdlog::debug("SPoE init with {} address(es), {} mode", drives_.size(),
                config_.mode == SpoeMode::kControl ? "Control" : "Monitor");
  return {};
}

void SpoeFieldbusDriver::connectAndIdentify(Drive& drive) {
  drive.info = SlaveInfo{};
  drive.state.store(0);
  if (auto connected = drive.connection.connect(drive.host, config_.port, config_.connectTimeout);
      !connected) {
    spdlog::warn("{}. The device keeps its position with no state known.", connected.error());
    return;
  }

  const auto info = request(drive, MessageType::kServerInfo, {});
  if (!info || info->data.size() < 3) {
    spdlog::warn("SPoE {}: no server information: {}", drive.host,
                 info ? "the reply is too short" : info.error());
    drive.connection.close();
    return;
  }
  // The firmware writes the version low byte first.
  const auto version = static_cast<uint16_t>(info->data[0] | (info->data[1] << 8));
  if (version != kSupportedProtocolVersion) {
    spdlog::error("SPoE {}: protocol version 0x{:04X} is not supported, only 0x{:04X} is",
                  drive.host, version, kSupportedProtocolVersion);
    drive.connection.close();
    return;
  }
  spdlog::info("SPoE {}: protocol version 0x{:04X}, PDO mode {}", drive.host, version,
               info->data[2]);

  const auto state = request(drive, MessageType::kStateRead, {});
  if (!state || state->data.empty()) {
    spdlog::warn("SPoE {}: no state: {}", drive.host, state ? "the reply is empty" : state.error());
    return;
  }
  drive.state.store(state->data[0]);

  const EtherCatState current = alState(state->data[0]);
  if (current == EtherCatState::Init || current == EtherCatState::Boot) {
    spdlog::info(
        "SPoE {}: the identity is not readable in {}, because the firmware refuses SDO "
        "access there",
        drive.host, toString(current));
    return;
  }
  const auto identity = [this, &drive](uint8_t subindex) -> uint32_t {
    const auto bytes = readSdoFrom(drive, 0x1018, subindex);
    return bytes && bytes->size() >= 4 ? readU32(*bytes, 0) : 0;
  };
  drive.info.vendorId = identity(1);
  drive.info.productCode = identity(2);
  drive.info.revisionNumber = identity(3);
  drive.info.serialNumber = identity(4);
  if (const auto name = readSdoFrom(drive, 0x1008, 0); name) {
    const auto end = std::find(name->begin(), name->end(), uint8_t{0});
    drive.info.name.assign(name->begin(), end);
  }
}

std::expected<int, std::string> SpoeFieldbusDriver::scan() {
  const std::scoped_lock lock(controlPlaneMutex_);
  // A scan rebuilds every connection, so no exchange may run across it.
  stopExchanges();
  layout_ = PdoLayout{};
  for (const auto& drive : drives_) {
    connectAndIdentify(*drive);
  }
  return static_cast<int>(drives_.size());
}

SlaveInfo SpoeFieldbusDriver::slaveInfo(uint16_t position) const {
  const std::scoped_lock lock(controlPlaneMutex_);
  const Drive* found = driveAt(position);
  return found != nullptr ? found->info : SlaveInfo{};
}

uint16_t SpoeFieldbusDriver::slaveState(uint16_t position) const {
  const Drive* found = driveAt(position);
  return found != nullptr ? found->state.load() : 0;
}

uint16_t SpoeFieldbusDriver::mailboxProtocols(uint16_t position) const {
  // SPoE carries SDO access and file transfer, which are what the CoE and FoE bits stand for.
  constexpr uint16_t kCoe = 0x04;
  constexpr uint16_t kFoe = 0x08;
  return driveAt(position) != nullptr ? static_cast<uint16_t>(kCoe | kFoe) : 0;
}

std::expected<uint32_t, std::string> SpoeFieldbusDriver::assignedBits(Drive& drive,
                                                                      uint16_t assignIndex) {
  // The assignment object lists the PDOs in use, and each PDO's subindex 0 counts its entries.
  // Entries beyond that count can hold stale mappings, so they are never read.
  const auto readU8 = [this, &drive](uint16_t index,
                                     uint8_t subindex) -> std::expected<uint8_t, std::string> {
    auto bytes = readSdoFrom(drive, index, subindex);
    if (!bytes) {
      return std::unexpected(bytes.error());
    }
    if (bytes->empty()) {
      return std::unexpected(
          std::format("SPoE {}: 0x{:04X}:{:02X} is empty", drive.host, index, subindex));
    }
    return (*bytes)[0];
  };
  const auto pdoCount = readU8(assignIndex, 0);
  if (!pdoCount) {
    return std::unexpected(pdoCount.error());
  }
  uint32_t bits = 0;
  for (uint8_t i = 1; i <= *pdoCount; ++i) {
    const auto pdo = readSdoFrom(drive, assignIndex, i);
    if (!pdo || pdo->size() < 2) {
      return std::unexpected(
          pdo ? std::format("SPoE {}: 0x{:04X}:{:02X} is too short", drive.host, assignIndex, i)
              : pdo.error());
    }
    const uint16_t pdoIndex = readU16(*pdo, 0);
    const auto entryCount = readU8(pdoIndex, 0);
    if (!entryCount) {
      return std::unexpected(entryCount.error());
    }
    for (uint8_t entry = 1; entry <= *entryCount; ++entry) {
      const auto mapping = readSdoFrom(drive, pdoIndex, entry);
      if (!mapping || mapping->empty()) {
        return std::unexpected(
            mapping ? std::format("SPoE {}: 0x{:04X}:{:02X} is empty", drive.host, pdoIndex, entry)
                    : mapping.error());
      }
      // A mapping entry is index u16, subindex u8 and bit length u8, low byte first.
      bits += (*mapping)[0];
    }
  }
  return bits;
}

std::expected<void, std::string> SpoeFieldbusDriver::configureProcessData() {
  const std::scoped_lock lock(controlPlaneMutex_);
  stopExchanges();
  PdoLayout layout;
  uint16_t position = 0;
  for (const auto& drive : drives_) {
    ++position;
    drive->io = SlaveIo{.slavePosition = position,
                        .outputOffset = layout.outputBytes,
                        .outputBytes = 0,
                        .inputOffset = layout.inputBytes,
                        .inputBytes = 0};
    if (drive->state.load() == 0 || !drive->connection.isOpen()) {
      layout.slaves.push_back(drive->io);
      continue;
    }
    const auto outputBits = assignedBits(*drive, 0x1C12);
    if (!outputBits) {
      return std::unexpected(outputBits.error());
    }
    const auto inputBits = assignedBits(*drive, 0x1C13);
    if (!inputBits) {
      return std::unexpected(inputBits.error());
    }
    drive->io.outputBytes = (*outputBits + 7) / 8;
    drive->io.inputBytes = (*inputBits + 7) / 8;

    const PdoMode mode = config_.mode == SpoeMode::kControl ? PdoMode::kControl : PdoMode::kMonitor;
    const std::vector<uint8_t> modeByte{static_cast<uint8_t>(mode)};
    const auto modeSet = request(*drive, MessageType::kPdoControl, modeByte);
    if (!modeSet) {
      return std::unexpected(modeSet.error());
    }
    if (modeSet->status != 0) {
      return std::unexpected(std::format("SPoE {}: the drive refused PDO mode {}", drive->host,
                                         static_cast<int>(mode)));
    }
    if (config_.mode == SpoeMode::kControl) {
      const uint32_t ms = config_.watchdogMs;
      const std::vector<uint8_t> timeout{static_cast<uint8_t>(ms), static_cast<uint8_t>(ms >> 8),
                                         static_cast<uint8_t>(ms >> 16),
                                         static_cast<uint8_t>(ms >> 24)};
      if (const auto set = request(*drive, MessageType::kWatchdogTimeout, timeout); !set) {
        return std::unexpected(set.error());
      }
    }

    drive->outputs.reset(drive->io.outputBytes);
    // The queue holds more than the limit, so that a burst the RT thread has not trimmed yet still
    // fits. The RT thread trims it back to the limit every cycle.
    drive->inputs.reset(drive->io.inputBytes, 2 * kInputQueueLimit);
    drive->assembler.reset(drive->io.inputBytes);
    drive->droppedFrames.store(0);
    layout.outputBytes += drive->io.outputBytes;
    layout.inputBytes += drive->io.inputBytes;
    layout.expectedWkc += (drive->io.outputBytes > 0 ? 2 : 0) + (drive->io.inputBytes > 0 ? 1 : 0);
    layout.slaves.push_back(drive->io);
  }
  layout_ = layout;
  for (const auto& drive : drives_) {
    if (drive->io.outputBytes > 0 || drive->io.inputBytes > 0) {
      Drive& target = *drive;
      drive->exchange = std::jthread(
          [this, &target](const std::stop_token& stopToken) { runExchange(target, stopToken); });
    }
  }
  spdlog::info("SPoE process data: {} output bytes, {} input bytes", layout_.outputBytes,
               layout_.inputBytes);
  return {};
}

void SpoeFieldbusDriver::runExchange(Drive& drive, const std::stop_token& stopToken) {
  // In Monitor mode the request carries no outputs, as the specification asks. The drive still
  // needs the request: it is how the drive sees that the client is there.
  const bool control = config_.mode == SpoeMode::kControl;
  auto next = std::chrono::steady_clock::now();
  while (!stopToken.stop_requested()) {
    next += config_.exchangePeriod;
    const std::span<const uint8_t> outputs =
        control ? drive.outputs.read() : std::span<const uint8_t>{};
    auto reply =
        drive.connection.request(MessageType::kPdoFrame, 0, outputs, config_.requestTimeout);
    if (!reply) {
      drive.exchanging.store(false);
      if (reply.error().kind == ConnectionError::Kind::kClosed) {
        drive.state.store(0);
        // Only a rescan reconnects. Wait for it without spinning.
        for (int i = 0; i < 10 && !stopToken.stop_requested(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        next = std::chrono::steady_clock::now();
      }
      continue;
    }
    // The firmware answers status 1 while no PDO mode is set, which a drive that restarted has.
    drive.exchanging.store(reply->status == 0);
    if (reply->status == 0) {
      const std::vector<uint8_t> frames = drive.assembler.add(reply->data);
      const std::size_t frameBytes = drive.io.inputBytes;
      for (std::size_t offset = 0; frameBytes > 0 && offset + frameBytes <= frames.size();
           offset += frameBytes) {
        if (!drive.inputs.push(std::span<const uint8_t>(frames).subspan(offset, frameBytes))) {
          drive.droppedFrames.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (next > now) {
      std::this_thread::sleep_until(next);
    } else {
      next = now;
    }
  }
}

PdoLayout SpoeFieldbusDriver::processDataLayout() {
  const std::scoped_lock lock(controlPlaneMutex_);
  return layout_;
}

int SpoeFieldbusDriver::exchangeProcessData(std::span<const uint8_t> outputs,
                                            std::span<uint8_t> inputs) {
  // The RT side. It hands over the newest outputs, takes one input frame per drive, and never
  // waits: the network round trip runs on each drive's exchange thread.
  const bool control = config_.mode == SpoeMode::kControl;
  int workingCounter = 0;
  for (const auto& drive : drives_) {
    const SlaveIo& io = drive->io;
    if (control && io.outputBytes > 0 && io.outputOffset + io.outputBytes <= outputs.size()) {
      drive->outputs.write(outputs.subspan(io.outputOffset, io.outputBytes));
    }
    if (io.inputBytes > 0 && io.inputOffset + io.inputBytes <= inputs.size()) {
      const std::size_t dropped = drive->inputs.dropOldest(kInputQueueLimit);
      if (dropped > 0) {
        drive->droppedFrames.fetch_add(dropped, std::memory_order_relaxed);
      }
      // No new frame leaves the previous inputs in place, as a lost EtherCAT frame does.
      drive->inputs.pop(inputs.subspan(io.inputOffset, io.inputBytes));
    }
    if (drive->exchanging.load(std::memory_order_relaxed)) {
      workingCounter += workingCounterContribution(alState(drive->state.load()), io.outputBytes > 0,
                                                   io.inputBytes > 0);
    }
  }
  return workingCounter;
}

void SpoeFieldbusDriver::stop() { releaseDrives(); }

void SpoeFieldbusDriver::releaseDrives() {
  stopExchanges();
  // Without a PDO mode, a Control-mode drive has no watchdog to trip, so a clean shutdown does not
  // fault it. The firmware runs the watchdog in Control mode only (`check_spoe_heartbeat`).
  const std::vector<uint8_t> none{static_cast<uint8_t>(PdoMode::kNone)};
  for (const auto& drive : drives_) {
    if (drive->connection.isOpen()) {
      (void)request(*drive, MessageType::kPdoControl, none);
    }
  }
  closeAll();
}

std::expected<std::vector<FieldbusDriver::SlaveStateRaw>, std::string>
SpoeFieldbusDriver::readStates(const std::vector<uint16_t>& positions) {
  std::vector<SlaveStateRaw> states;
  states.reserve(positions.size());
  for (const uint16_t position : positions) {
    Drive* found = driveAt(position);
    if (found == nullptr) {
      return std::unexpected(std::format("no SPoE device at position {}", position));
    }
    // SPoE has no error indicator and no AL status code. A drive that does not answer reads as
    // "no state known", and the others are still read.
    const auto reply = request(*found, MessageType::kStateRead, {});
    if (reply && !reply->data.empty()) {
      found->state.store(reply->data[0]);
    } else {
      found->state.store(0);
    }
    states.push_back(SlaveStateRaw{.alStatus = found->state.load(), .alStatusCode = 0});
  }
  return states;
}

std::expected<std::vector<uint8_t>, std::string> SpoeFieldbusDriver::readSdoFrom(Drive& drive,
                                                                                 uint16_t index,
                                                                                 uint8_t subindex) {
  const auto reply = request(drive, MessageType::kSdoRead, address(index, subindex));
  if (!reply) {
    return std::unexpected(reply.error());
  }
  if (reply->status != 0) {
    return std::unexpected(std::format("SPoE {}: SDO read 0x{:04X}:{:02X} failed (status {}: {})",
                                       drive.host, index, subindex, reply->status,
                                       sdoStatusName(reply->status)));
  }
  return reply->data;
}

std::expected<std::vector<uint8_t>, std::string> SpoeFieldbusDriver::readSdo(uint16_t slavePosition,
                                                                             uint16_t index,
                                                                             uint8_t subindex) {
  Drive* found = driveAt(slavePosition);
  if (found == nullptr) {
    return std::unexpected(std::format("no SPoE device at position {}", slavePosition));
  }
  return readSdoFrom(*found, index, subindex);
}

std::expected<void, std::string> SpoeFieldbusDriver::writeSdo(uint16_t slavePosition,
                                                              uint16_t index, uint8_t subindex,
                                                              std::span<const uint8_t> data) {
  Drive* found = driveAt(slavePosition);
  if (found == nullptr) {
    return std::unexpected(std::format("no SPoE device at position {}", slavePosition));
  }
  // Index, subindex and value length come first, so six bytes of the frame are not value.
  constexpr std::size_t kWriteHeader = 6;
  if (data.size() > kMaxDataSize - kWriteHeader) {
    return std::unexpected(
        std::format("SPoE {}: SDO write 0x{:04X}:{:02X} of {} bytes exceeds the {}-byte limit",
                    found->host, index, subindex, data.size(), kMaxDataSize - kWriteHeader));
  }
  std::vector<uint8_t> payload = address(index, subindex);
  payload.push_back(static_cast<uint8_t>(data.size()));
  payload.push_back(static_cast<uint8_t>(data.size() >> 8));
  payload.insert(payload.end(), data.begin(), data.end());
  const auto reply = request(*found, MessageType::kSdoWrite, payload);
  if (!reply) {
    return std::unexpected(reply.error());
  }
  if (reply->status != 0) {
    return std::unexpected(std::format("SPoE {}: SDO write 0x{:04X}:{:02X} failed (status {}: {})",
                                       found->host, index, subindex, reply->status,
                                       sdoStatusName(reply->status)));
  }
  return {};
}

std::expected<OdRead, std::string> SpoeFieldbusDriver::readObjectDictionary(
    uint16_t slavePosition) {
  Drive* found = driveAt(slavePosition);
  if (found == nullptr) {
    return std::unexpected(std::format("no SPoE device at position {}", slavePosition));
  }
  Drive& target = *found;

  const auto list = request(target, MessageType::kParamList, {});
  if (!list) {
    return std::unexpected(list.error());
  }
  std::vector<uint16_t> indexes;
  for (std::size_t offset = 0; offset + 2 <= list->data.size(); offset += 2) {
    indexes.push_back(readU16(list->data, offset));
  }

  // The bulk list is fast and is not trusted alone. The status of each middle packet comes from an
  // uninitialised variable in `AppSockIf_ReadObjectInfo`. When it happens to read as BUSY, the
  // firmware sends no entries for that packet and still moves past them. So every gap the bulk
  // list leaves is filled below, one description at a time.
  std::map<std::pair<uint16_t, uint8_t>, ParameterEntry> entriesByAddress;
  int lostPackets = 0;
  if (const auto first = request(target, MessageType::kParamFullDesc, {}, kPacketFirst);
      first && (first->status & 0xFF) == kReplyAck) {
    for (int packet = 0; packet < kMaxParameterListPackets; ++packet) {
      const auto reply = request(target, MessageType::kParamFullDesc, {}, kPacketMiddle);
      if (!reply) {
        spdlog::warn("SPoE {}: parameter list stopped: {}", target.host, reply.error());
        break;
      }
      if (reply->data.empty() && (reply->status & 0xFF) == kReplyBusy) {
        ++lostPackets;
      }
      for (auto& decoded : decodeParameterEntries(reply->data)) {
        const auto key = std::pair{decoded.entry.index, decoded.entry.subindex};
        entriesByAddress.insert_or_assign(key, std::move(decoded));
      }
      if (static_cast<uint8_t>(reply->status >> 8) == kPacketLast ||
          (reply->status & 0xFF) == kReplyError) {
        break;
      }
    }
  }

  // A description that comes back cleared means the firmware has no such entry. That is an
  // answer, not a loss. Only a request that gets no answer counts as missing.
  int missing = 0;
  int filled = 0;
  const auto describe = [&](MessageType type, uint16_t index,
                            uint8_t subindex) -> std::optional<ParameterEntry> {
    const auto reply = request(target, type, address(index, subindex));
    if (!reply) {
      ++missing;
      return std::nullopt;
    }
    auto decoded = decodeParameterEntries(reply->data);
    const auto match = std::ranges::find_if(decoded, [index, subindex](const ParameterEntry& e) {
      return e.entry.index == index && e.entry.subindex == subindex;
    });
    if (match == decoded.end()) {
      return std::nullopt;
    }
    ++filled;
    return std::move(*match);
  };
  for (const uint16_t index : indexes) {
    auto object = entriesByAddress.find({index, 0});
    if (object == entriesByAddress.end()) {
      auto described = describe(MessageType::kParamDesc, index, 0);
      if (!described) {
        continue;
      }
      object = entriesByAddress.emplace(std::pair{index, uint8_t{0}}, std::move(*described)).first;
    }
    if (!isOpenObject(object->second.entry.objectCode)) {
      continue;
    }
    const auto count = std::min<uint32_t>(object->second.subindexCount, 0xFF);
    for (uint32_t subindex = 1; subindex <= count; ++subindex) {
      const auto key = std::pair{index, static_cast<uint8_t>(subindex)};
      if (entriesByAddress.contains(key)) {
        continue;
      }
      if (auto described = describe(MessageType::kParamSubDesc, index, key.second); described) {
        entriesByAddress.emplace(key, std::move(*described));
      }
    }
  }

  OdRead od;
  od.entries.reserve(entriesByAddress.size());
  for (auto& [key, decoded] : entriesByAddress) {
    od.entries.push_back(std::move(decoded.entry));
  }
  od.missingEntries = missing;
  spdlog::debug(
      "SPoE {}: object dictionary has {} entries in {} objects ({} lost packets, {} entries filled "
      "one by one, {} missing)",
      target.host, od.entries.size(), indexes.size(), lostPackets, filled, missing);
  return od;
}

namespace {

// The FoE error codes the firmware puts in the low status byte of a file reply
// (`SqiBridgeFoeError_t`, `sqi_bridge_common.h`). A successful packet carries kReplyAck instead.
constexpr uint8_t kFoeNotFound = 0x01;
constexpr uint8_t kFoePacketNumber = 0x05;
constexpr uint8_t kFoeBusy = 0x0C;
constexpr uint8_t kFoeTimeout = 0x0E;

// File packets carry at most this much data. The size is proven on hardware, and the firmware
// accepts up to SQI_ACYCLIC_MAX_FOE_SIZE (1024).
constexpr std::size_t kFileChunk = 500;

// The firmware stores this file in its own flash, and answers its first packet in BOOT with the
// return value of `storage_prepare_for_writing`, where 0 is success, rather than with kReplyAck.
constexpr std::string_view kComFirmwareFile = "com_firmware.bin";

// The firmware sends no file larger than this. The bound only stops a drive that never sends the
// last packet.
constexpr int kMaxFilePackets = 200000;

FoeError foeErrorFromStatus(const std::string& host, const std::string& filename, uint8_t code) {
  const auto message = [&](std::string_view reason) {
    return std::format("SPoE {}: file '{}' failed: {} (code 0x{:02X})", host, filename, reason,
                       code);
  };
  switch (code) {
    case kFoeNotFound:
      return FoeError{.kind = FoeErrorKind::FileNotFound,
                      .retry = Retry::Permanent,
                      .message = message("file not found")};
    case kFoePacketNumber:
      return FoeError{.kind = FoeErrorKind::PacketMismatch,
                      .retry = Retry::Transient,
                      .message = message("packet number mismatch")};
    case kFoeBusy:
      return FoeError{
          .kind = FoeErrorKind::Protocol, .retry = Retry::Transient, .message = message("busy")};
    case kFoeTimeout:
      return FoeError{.kind = FoeErrorKind::NoResponse,
                      .retry = Retry::Transient,
                      .message = message("timeout inside the drive")};
    case kReplyError:
      return FoeError{.kind = FoeErrorKind::Protocol,
                      .retry = Retry::Transient,
                      .message = message("communication bridge error")};
    default:
      return FoeError{.kind = FoeErrorKind::Protocol,
                      .retry = Retry::Permanent,
                      .message = message("file error")};
  }
}

}  // namespace

std::expected<Frame, FoeError> SpoeFieldbusDriver::filePacket(Drive& drive, MessageType type,
                                                              uint8_t packetState,
                                                              std::span<const uint8_t> data) {
  auto reply = drive.connection.request(type, packetState, data, config_.fileTimeout);
  if (!reply) {
    if (reply.error().kind == ConnectionError::Kind::kClosed) {
      drive.state.store(0);
    }
    return std::unexpected(FoeError{.kind = FoeErrorKind::NoResponse,
                                    .retry = Retry::Transient,
                                    .message = reply.error().message});
  }
  return std::move(*reply);
}

std::expected<std::vector<uint8_t>, FoeError> SpoeFieldbusDriver::readFileFrom(
    Drive& drive, const std::string& filename) {
  const std::vector<uint8_t> name(filename.begin(), filename.end());
  std::vector<uint8_t> content;
  auto reply = filePacket(drive, MessageType::kFileRead, kPacketFirst, name);
  for (int packet = 0; packet < kMaxFilePackets; ++packet) {
    if (!reply) {
      return std::unexpected(reply.error());
    }
    const auto code = static_cast<uint8_t>(reply->status & 0xFF);
    if (code != kReplyAck) {
      return std::unexpected(foeErrorFromStatus(drive.host, filename, code));
    }
    content.insert(content.end(), reply->data.begin(), reply->data.end());
    if (static_cast<uint8_t>(reply->status >> 8) == kPacketLast) {
      return content;
    }
    reply = filePacket(drive, MessageType::kFileRead, kPacketMiddle, {});
  }
  return std::unexpected(
      FoeError{.kind = FoeErrorKind::Protocol,
               .retry = Retry::Permanent,
               .message = std::format("SPoE {}: file '{}' never ended", drive.host, filename)});
}

std::expected<std::vector<uint8_t>, FoeError> SpoeFieldbusDriver::readFile(
    uint16_t slavePosition, const std::string& filename) {
  Drive* found = driveAt(slavePosition);
  if (found == nullptr) {
    return std::unexpected(
        FoeError{.kind = FoeErrorKind::Protocol,
                 .retry = Retry::Permanent,
                 .message = std::format("no SPoE device at position {}", slavePosition)});
  }
  auto content = readFileFrom(*found, filename);
  if (!content || !content->empty() || filename == "fs-getlist") {
    return content;
  }
  // An empty read can be a missing file, and the firmware source does not show what the SoC answers
  // for one. The file list tells the two apart.
  const auto list = readFileFrom(*found, "fs-getlist");
  if (!list) {
    return content;
  }
  const std::string text(list->begin(), list->end());
  for (std::size_t begin = 0; begin < text.size();) {
    const std::size_t end = std::min(text.find('\n', begin), text.size());
    const std::string line = text.substr(begin, end - begin);
    if (line.substr(0, line.find(',')) == filename) {
      return content;
    }
    begin = end + 1;
  }
  return std::unexpected(
      FoeError{.kind = FoeErrorKind::FileNotFound,
               .retry = Retry::Permanent,
               .message = std::format("SPoE {}: file '{}' not found", found->host, filename)});
}

std::expected<void, FoeError> SpoeFieldbusDriver::writeFile(uint16_t slavePosition,
                                                            const std::string& filename,
                                                            std::span<const uint8_t> data) {
  Drive* found = driveAt(slavePosition);
  if (found == nullptr) {
    return std::unexpected(
        FoeError{.kind = FoeErrorKind::Protocol,
                 .retry = Retry::Permanent,
                 .message = std::format("no SPoE device at position {}", slavePosition)});
  }
  Drive& drive = *found;
  const std::vector<uint8_t> name(filename.begin(), filename.end());
  const auto first = filePacket(drive, MessageType::kFileWrite, kPacketFirst, name);
  if (!first) {
    return std::unexpected(first.error());
  }
  const auto firstCode = static_cast<uint8_t>(first->status & 0xFF);
  const bool storageReady = filename == kComFirmwareFile && firstCode == 0;
  if (firstCode != kReplyAck && !storageReady) {
    return std::unexpected(foeErrorFromStatus(drive.host, filename, firstCode));
  }
  // An empty file still sends its last packet, which is what closes the file on the drive.
  std::size_t offset = 0;
  do {
    const std::size_t chunk = std::min(kFileChunk, data.size() - offset);
    const bool last = offset + chunk >= data.size();
    const auto reply = filePacket(drive, MessageType::kFileWrite,
                                  last ? kPacketLast : kPacketMiddle, data.subspan(offset, chunk));
    if (!reply) {
      return std::unexpected(reply.error());
    }
    if (const auto code = static_cast<uint8_t>(reply->status & 0xFF); code != kReplyAck) {
      return std::unexpected(foeErrorFromStatus(drive.host, filename, code));
    }
    offset += chunk;
  } while (offset < data.size());
  return {};
}

std::expected<void, std::string> SpoeFieldbusDriver::readRegister(uint16_t /*slavePosition*/,
                                                                  uint16_t /*address*/,
                                                                  std::span<uint8_t> /*data*/) {
  return std::unexpected("ESC registers do not exist over SPoE");
}

std::expected<void, std::string> SpoeFieldbusDriver::writeRegister(
    uint16_t /*slavePosition*/, uint16_t /*address*/, std::span<const uint8_t> /*data*/) {
  return std::unexpected("ESC registers do not exist over SPoE");
}

void SpoeFieldbusDriver::transitionToState(const std::vector<uint16_t>& positions,
                                           std::optional<EtherCatState> requiredState,
                                           EtherCatState targetState,
                                           std::chrono::steady_clock::duration timeout,
                                           std::chrono::steady_clock::duration /*resendInterval*/,
                                           std::function<void()> /*tick*/,
                                           std::function<bool()> shouldAbort) {
  if (const auto refusal = stateChangeRefusal(); refusal) {
    spdlog::error("{}", *refusal);
    return;
  }
  // `AppUtil_ChangeState` changes the state before the firmware answers. So there is nothing to
  // resend and nothing to wait for, and the reply carries the state the drive is in.
  const auto stateTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
  const std::vector<uint8_t> command{static_cast<uint8_t>(targetState)};
  for (const uint16_t position : positions) {
    if (shouldAbort && shouldAbort()) {
      return;
    }
    Drive* found = driveAt(position);
    if (found == nullptr) {
      continue;
    }
    if (requiredState && alState(found->state.load()) != *requiredState) {
      continue;
    }
    const auto reply =
        found->connection.request(MessageType::kStateControl, 0, command, stateTimeout);
    if (!reply) {
      if (reply.error().kind == ConnectionError::Kind::kClosed) {
        found->state.store(0);
      }
      spdlog::error("SPoE {}: state change to {} failed: {}", found->host, toString(targetState),
                    reply.error().message);
      continue;
    }
    // Success is the state the drive reports, not the status. `AppUtil_ChangeState` puts a bool
    // in the status, and firmware versions disagree on its value.
    if (reply->data.empty()) {
      spdlog::error("SPoE {}: the reply to the state change to {} carries no state", found->host,
                    toString(targetState));
      continue;
    }
    found->state.store(reply->data[0]);
    if (alState(reply->data[0]) != targetState) {
      spdlog::error("SPoE {}: the drive stayed in {} instead of changing to {}", found->host,
                    toString(alState(reply->data[0])), toString(targetState));
    }
  }
}

uint64_t SpoeFieldbusDriver::droppedInputFrames() const {
  return std::accumulate(drives_.begin(), drives_.end(), uint64_t{0},
                         [](uint64_t sum, const std::unique_ptr<Drive>& drive) {
                           return sum + drive->droppedFrames.load(std::memory_order_relaxed);
                         });
}

std::expected<void, std::string> SpoeFieldbusDriver::locate(uint16_t slavePosition, bool on) {
  Drive* found = driveAt(slavePosition);
  if (found == nullptr) {
    return std::unexpected(std::format("no SPoE device at position {}", slavePosition));
  }
  // The status echoes the LED state, 1 for blinking and 0 for stopped, so any answer is success.
  const std::vector<uint8_t> command{static_cast<uint8_t>(on ? 1 : 0)};
  if (auto reply = request(*found, MessageType::kDeviceLocate, command); !reply) {
    return std::unexpected(reply.error());
  }
  return {};
}

std::expected<FieldbusDriver::FirmwareActivation, std::string> SpoeFieldbusDriver::activateFirmware(
    uint16_t slavePosition, std::chrono::steady_clock::duration timeout) {
  Drive* found = driveAt(slavePosition);
  if (found == nullptr) {
    return std::unexpected(std::format("no SPoE device at position {}", slavePosition));
  }
  Drive& drive = *found;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  // The firmware answers, then resets the netX into its update mode, which loads the COM firmware
  // from flash and restarts the drive. The restart also starts the SoC firmware written in BOOT.
  if (auto reply = request(drive, MessageType::kFirmwareUpdate, {}); !reply) {
    return std::unexpected(reply.error());
  }
  // The reset follows the answer by `ulTimeToReset`, 1000 ms. A connection made before it would
  // reach the old firmware and look like a finished restart, so first wait for the drive to stop
  // answering.
  while (request(drive, MessageType::kServerInfo, {})) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return std::unexpected(std::format(
          "SPoE {}: the drive kept answering and did not restart for the firmware update",
          drive.host));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  drive.connection.close();
  drive.state.store(0);
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!drive.connection.connect(drive.host, config_.port, config_.connectTimeout)) {
      continue;
    }
    const auto info = request(drive, MessageType::kServerInfo, {});
    const auto state =
        info ? request(drive, MessageType::kStateRead, {}) : std::unexpected(info.error());
    if (state && !state->data.empty()) {
      drive.state.store(state->data[0]);
      spdlog::info("SPoE {}: the drive answers again after the firmware update, in {}", drive.host,
                   toString(alState(state->data[0])));
      return FirmwareActivation::kRestarted;
    }
    drive.connection.close();
  }
  return std::unexpected(
      std::format("SPoE {}: the drive did not answer again within {} s of the firmware update",
                  drive.host, std::chrono::duration_cast<std::chrono::seconds>(timeout).count()));
}

std::optional<std::string> SpoeFieldbusDriver::stateChangeRefusal() const {
  if (config_.mode == SpoeMode::kMonitor) {
    return "SPoE is in Monitor mode, where the PLC owns the drive's state. Set "
           "fieldbus.spoe.mode to \"control\" to change the state from Motion Master.";
  }
  return std::nullopt;
}

}  // namespace mm::comm::spoe
