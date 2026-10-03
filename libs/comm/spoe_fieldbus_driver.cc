#include "comm/spoe_fieldbus_driver.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "comm/spoe_connection.h"

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

// `AppUtil_ChangeState` returns a bool, so a state change reports 1 for success.
constexpr uint16_t kStateChangeSucceeded = 1;

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

SpoeFieldbusDriver::~SpoeFieldbusDriver() { closeAll(); }

void SpoeFieldbusDriver::closeAll() {
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

std::expected<void, std::string> SpoeFieldbusDriver::configureProcessData() {
  return std::unexpected("process data over SPoE is not implemented yet");
}

PdoLayout SpoeFieldbusDriver::processDataLayout() { return {}; }

int SpoeFieldbusDriver::exchangeProcessData(std::span<const uint8_t> /*outputs*/,
                                            std::span<uint8_t> /*inputs*/) {
  return 0;
}

void SpoeFieldbusDriver::stop() { closeAll(); }

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

std::expected<std::vector<uint8_t>, FoeError> SpoeFieldbusDriver::readFile(
    uint16_t /*slavePosition*/, const std::string& /*filename*/) {
  return std::unexpected(FoeError{.kind = FoeErrorKind::Protocol,
                                  .retry = Retry::Permanent,
                                  .message = "file transfer over SPoE is not implemented yet"});
}

std::expected<void, FoeError> SpoeFieldbusDriver::writeFile(uint16_t /*slavePosition*/,
                                                            const std::string& /*filename*/,
                                                            std::span<const uint8_t> /*data*/) {
  return std::unexpected(FoeError{.kind = FoeErrorKind::Protocol,
                                  .retry = Retry::Permanent,
                                  .message = "file transfer over SPoE is not implemented yet"});
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
    if (!reply->data.empty()) {
      found->state.store(reply->data[0]);
    }
    if (reply->status != kStateChangeSucceeded) {
      spdlog::error("SPoE {}: the drive refused the state change to {}", found->host,
                    toString(targetState));
    }
  }
}

std::optional<std::string> SpoeFieldbusDriver::stateChangeRefusal() const {
  if (config_.mode == SpoeMode::kMonitor) {
    return "SPoE is in Monitor mode, where the PLC owns the drive's state. Set "
           "fieldbus.spoe.mode to \"control\" to change the state from Motion Master.";
  }
  return std::nullopt;
}

}  // namespace mm::comm::spoe
