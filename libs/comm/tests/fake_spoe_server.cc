#include "fake_spoe_server.h"

#include <algorithm>
#include <array>
#include <asio.hpp>
#include <chrono>
#include <cstdio>
#include <format>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mm::comm::testing {

namespace {

using asio::ip::tcp;

// Every asynchronous call answers with an error code. Nothing in the fake throws.
constexpr auto kNoThrow = asio::as_tuple(asio::use_awaitable);

// `pop_from_pdo_fifo_buffer` returns at most this many bytes of buffered inputs in one reply.
constexpr std::size_t kInputPopLimit = 500;

// `pdo_buf` in `cyclic_communication.c`. A push that does not fit empties the buffer first.
constexpr std::size_t kInputBufferSize = 512;

// `SPOE_WATCHDOG_TIMEOUT_MIN_MS`. The firmware raises a smaller value to this one.
constexpr uint32_t kWatchdogMinimumMs = 50;

// The firmware writes this status for a process-data request with no PDO mode set, and for a PDO
// control request whose data is not exactly one byte.
constexpr uint16_t kPdoError = 1;

// The split reply sends its parts this far apart, so that each part leaves as its own segment.
constexpr std::chrono::milliseconds kSplitGap{2};

uint16_t readU16(std::span<const uint8_t> data, std::size_t offset) {
  // The firmware copies from its receive buffer whatever the length says. A short request reads
  // stale bytes there. The fake reads zeros, which a test can predict.
  uint16_t value = 0;
  for (std::size_t i = 0; i < 2 && offset + i < data.size(); ++i) {
    value |= static_cast<uint16_t>(data[offset + i] << (8 * i));
  }
  return value;
}

uint32_t readU32(std::span<const uint8_t> data, std::size_t offset) {
  return readU16(data, offset) | (static_cast<uint32_t>(readU16(data, offset + 2)) << 16);
}

void appendU16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

std::vector<uint8_t> encodeFrame(uint8_t type, uint16_t sequenceId, uint16_t status,
                                 uint16_t dataLength, std::span<const uint8_t> data) {
  const std::array<uint8_t, kSpoeHeaderSize> header{type,
                                                    static_cast<uint8_t>(sequenceId),
                                                    static_cast<uint8_t>(sequenceId >> 8),
                                                    static_cast<uint8_t>(status),
                                                    static_cast<uint8_t>(status >> 8),
                                                    static_cast<uint8_t>(dataLength),
                                                    static_cast<uint8_t>(dataLength >> 8)};
  std::vector<uint8_t> frame(header.begin(), header.end());
  frame.insert(frame.end(), data.begin(), data.end());
  return frame;
}

// Errors are of no interest here: the socket is finished either way.
void closeSocket(tcp::socket& socket) {
  asio::error_code ignored;
  (void)socket.shutdown(tcp::socket::shutdown_both, ignored);
  (void)socket.close(ignored);
}

struct Request {
  uint8_t type = 0;
  uint16_t sequenceId = 0;
  uint16_t status = 0;
  std::vector<uint8_t> data;
};

struct Reply {
  uint16_t status = 0;
  std::vector<uint8_t> data;
};

}  // namespace

struct FakeSpoeServer::Impl {
  // Declared first, so that it is destroyed last: the sockets below need its services.
  asio::io_context io;
  tcp::acceptor acceptor{io};
  uint16_t port = 0;

  // Touched on the I/O thread only.
  std::shared_ptr<tcp::socket> client;
  std::vector<tcp::socket> ignoredClients;

  mutable std::mutex mutex;
  std::map<std::pair<uint16_t, uint16_t>, std::vector<uint8_t>> objects;
  std::map<std::pair<uint16_t, uint16_t>, FakeSpoeEntry> descriptions;
  // Files, and the transfer in progress. The firmware keeps one transfer at a time.
  std::map<std::string, std::vector<uint8_t>> files;
  std::string readName;
  std::vector<uint8_t> readContent;
  std::size_t readOffset = 0;
  std::string writeName;
  std::vector<uint8_t> writeContent;
  bool writing = false;
  // The firmware update: requests counted, and the restart that follows.
  int firmwareUpdates = 0;
  std::chrono::milliseconds resetDelay{300};
  std::chrono::milliseconds restartDuration{500};
  std::chrono::steady_clock::time_point restartedAt{};
  bool restarting = false;
  // The parameter-list cursor, as `AppSockIf_ReadObjectInfo` keeps it in static variables.
  std::vector<uint16_t> listedIndexes;
  std::size_t nextIndexPosition = 0;
  uint16_t nextSubindex = 0;
  uint16_t maxSubindexLatch = 0;
  uint8_t state = kSpoeStatePreOp;
  bool refuseStateChanges = false;
  uint8_t pdoMode = kSpoePdoModeNone;
  bool locating = false;
  std::optional<uint32_t> watchdogTimeoutMs;
  bool spoeActive = true;
  std::vector<uint8_t> inputBuffer;
  std::vector<uint8_t> lastOutputs;
  std::optional<SpoeFault> nextFault;
  int requestsBeforeFault = 0;
  std::chrono::milliseconds replyDelay{0};
  int connections = 0;
  int ignoredConnections = 0;
  int requests = 0;
  int unmodelled = 0;

  std::thread thread;

  asio::awaitable<void> acceptLoop();
  asio::awaitable<void> serve(std::shared_ptr<tcp::socket> socket);

  // Called with `mutex` held. Returns no reply for a request the fake does not model.
  std::optional<Reply> handle(const Request& request);
  Reply readSdo(uint16_t index, uint16_t subindex) const;
  std::vector<uint16_t> indexList() const;
  uint16_t maxSubindex(uint16_t index) const;
  std::vector<uint8_t> encodeEntry(uint16_t index, uint16_t subindex) const;
  Reply paramFullDesc(uint8_t packetState);
  Reply fileRead(uint8_t packetState, std::span<const uint8_t> data);
  Reply fileWrite(uint8_t packetState, std::span<const uint8_t> data);
  std::vector<uint8_t> fileList() const;
  uint16_t sdoLookupStatus(uint16_t index, uint16_t subindex) const;
};

asio::awaitable<void> FakeSpoeServer::Impl::acceptLoop() {
  for (;;) {
    auto [ec, socket] = co_await acceptor.async_accept(kNoThrow);
    if (ec) {
      co_return;
    }
    asio::error_code ignored;
    {
      // While the drive restarts after a firmware update, nothing serves the port.
      const std::scoped_lock lock(mutex);
      if (restarting && std::chrono::steady_clock::now() < restartedAt) {
        closeSocket(socket);
        continue;
      }
      if (restarting) {
        // The new firmware comes up in PRE-OP, as the firmware does outside EtherCAT.
        restarting = false;
        state = kSpoeStatePreOp;
        pdoMode = kSpoePdoModeNone;
      }
    }
    // A split reply must leave in several segments. Nagle's algorithm would merge them again.
    (void)socket.set_option(tcp::no_delay(true), ignored);
    if (client) {
      // `Sock_SaveHandle` has no free slot. The connection stays open and is never read.
      {
        const std::scoped_lock lock(mutex);
        ++ignoredConnections;
      }
      ignoredClients.push_back(std::move(socket));
      continue;
    }
    {
      const std::scoped_lock lock(mutex);
      ++connections;
    }
    client = std::make_shared<tcp::socket>(std::move(socket));
    asio::co_spawn(io, serve(client), asio::detached);
  }
}

asio::awaitable<void> FakeSpoeServer::Impl::serve(std::shared_ptr<tcp::socket> socket) {
  std::vector<uint8_t> held;
  for (;;) {
    std::array<uint8_t, kSpoeHeaderSize> header{};
    if (auto [ec, n] = co_await asio::async_read(*socket, asio::buffer(header), kNoThrow); ec) {
      break;
    }
    Request request;
    request.type = header[0];
    request.sequenceId = readU16(header, 1);
    request.status = readU16(header, 3);
    const uint16_t length = readU16(header, 5);
    if (length > kSpoeMaxDataSize) {
      break;
    }
    request.data.resize(length);
    if (length > 0) {
      if (auto [ec, n] = co_await asio::async_read(*socket, asio::buffer(request.data), kNoThrow);
          ec) {
        break;
      }
    }

    std::optional<Reply> reply;
    std::optional<SpoeFault> fault;
    std::chrono::milliseconds delay{0};
    {
      const std::scoped_lock lock(mutex);
      ++requests;
      if (requestsBeforeFault > 0) {
        --requestsBeforeFault;
      } else {
        fault = std::exchange(nextFault, std::nullopt);
      }
      delay = replyDelay;
      reply = handle(request);
      if (!reply) {
        ++unmodelled;
      }
    }
    if (!reply) {
      break;
    }
    if (fault == SpoeFault::kLoseParamListPacket &&
        request.type == static_cast<uint8_t>(SpoeMessage::kParamFullDesc)) {
      reply->data.clear();
      reply->status = static_cast<uint16_t>((reply->status & 0xFF00) | kSpoeReplyBusy);
    }
    if (delay.count() > 0) {
      asio::steady_timer timer(io, delay);
      co_await timer.async_wait(kNoThrow);
    }

    uint16_t sequenceId = request.sequenceId;
    auto dataLength = static_cast<uint16_t>(reply->data.size());
    if (fault == SpoeFault::kWrongSequenceId) {
      sequenceId = static_cast<uint16_t>(sequenceId + 1);
    } else if (fault == SpoeFault::kOversizeLength) {
      dataLength = static_cast<uint16_t>(kSpoeMaxDataSize + 1);
    } else if (fault == SpoeFault::kNoReply) {
      continue;
    } else if (fault == SpoeFault::kDropConnection) {
      break;
    }
    std::vector<uint8_t> frame =
        encodeFrame(request.type, sequenceId, reply->status, dataLength, reply->data);
    if (fault == SpoeFault::kHoldReply) {
      held.insert(held.end(), frame.begin(), frame.end());
      continue;
    }
    frame.insert(frame.begin(), held.begin(), held.end());
    held.clear();

    if (fault == SpoeFault::kSplitReply) {
      // Cut inside the header and inside the data, because a client that reads the header with one
      // call and the data with another must handle both cuts.
      const std::array<std::size_t, 3> cuts{3, kSpoeHeaderSize + 1, frame.size()};
      std::size_t begin = 0;
      bool failed = false;
      for (const std::size_t cut : cuts) {
        const std::size_t end = std::min(cut, frame.size());
        if (end <= begin) {
          continue;
        }
        auto [ec, n] = co_await asio::async_write(
            *socket, asio::buffer(frame.data() + begin, end - begin), kNoThrow);
        if (ec) {
          failed = true;
          break;
        }
        begin = end;
        asio::steady_timer timer(io, kSplitGap);
        co_await timer.async_wait(kNoThrow);
      }
      if (failed) {
        break;
      }
      continue;
    }
    if (auto [ec, n] = co_await asio::async_write(*socket, asio::buffer(frame), kNoThrow); ec) {
      break;
    }
    if (request.type == static_cast<uint8_t>(SpoeMessage::kFirmwareUpdate)) {
      // `AppSockIf_StartUpdateReq` asks for the reset after `ulTimeToReset`. Until then the drive
      // reads nothing more, and the reset drops the connection without a word.
      std::chrono::milliseconds wait{0};
      {
        const std::scoped_lock lock(mutex);
        wait = resetDelay;
        restarting = true;
        restartedAt = std::chrono::steady_clock::now() + resetDelay + restartDuration;
      }
      asio::steady_timer timer(io, wait);
      co_await timer.async_wait(kNoThrow);
      break;
    }
  }
  closeSocket(*socket);
  if (client == socket) {
    client.reset();
  }
  co_return;
}

uint16_t FakeSpoeServer::Impl::sdoLookupStatus(uint16_t index, uint16_t subindex) const {
  if (objects.contains({index, subindex})) {
    return 0;
  }
  // `find_entry` in the netX object dictionary. The SoC answers for objects from 0x2001 up, and
  // which code it gives for a missing object is not confirmed.
  const auto sameIndex = objects.lower_bound({index, 0});
  const bool indexExists = sameIndex != objects.end() && sameIndex->first.first == index;
  return indexExists ? kSpoeSdoSubNotFound : kSpoeSdoNotFound;
}

Reply FakeSpoeServer::Impl::readSdo(uint16_t index, uint16_t subindex) const {
  if (state == kSpoeStateInit || state == kSpoeStateBoot) {
    return {.status = kSpoeSdoNotAllowedInState, .data = {}};
  }
  if (const uint16_t status = sdoLookupStatus(index, subindex); status != 0) {
    return {.status = status, .data = {}};
  }
  return {.status = 0, .data = objects.at({index, subindex})};
}

std::vector<uint16_t> FakeSpoeServer::Impl::indexList() const {
  // `sdoinfo_get_list` stops at `MAX_INDEX_LIST`, so a larger dictionary is cut short.
  std::vector<uint16_t> indexes;
  for (const auto& [address, entry] : descriptions) {
    if ((indexes.empty() || indexes.back() != address.first) &&
        indexes.size() < kSpoeMaxIndexList) {
      indexes.push_back(address.first);
    }
  }
  return indexes;
}

uint16_t FakeSpoeServer::Impl::maxSubindex(uint16_t index) const {
  uint16_t highest = 0;
  for (auto it = descriptions.lower_bound({index, 0});
       it != descriptions.end() && it->first.first == index; ++it) {
    highest = it->first.second;
  }
  return highest;
}

std::vector<uint8_t> FakeSpoeServer::Impl::encodeEntry(uint16_t index, uint16_t subindex) const {
  // `AppSockIf_GetObjectInfoBuf` clears the description and copies it whether the lookup found
  // the entry or not. What the lookup writes for a missing entry is not confirmed, so the fake
  // sends the cleared description.
  std::vector<uint8_t> bytes(kSpoeEntrySize, 0);
  const auto it = descriptions.find({index, subindex});
  if (it == descriptions.end()) {
    return bytes;
  }
  const FakeSpoeEntry& entry = it->second;
  const auto putU16 = [&bytes](std::size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
  };
  // `obj.value` is overwritten with the subindex count of subindex 0, which stays zero for every
  // other subindex.
  const uint32_t value = subindex == 0 ? maxSubindex(index) : 0;
  putU16(0, index);
  bytes[2] = static_cast<uint8_t>(subindex);
  putU16(4, entry.dataType);
  bytes[6] = entry.objectCode;
  putU16(8, entry.bitLength);
  putU16(10, entry.access);
  putU16(12, static_cast<uint16_t>(value));
  putU16(14, static_cast<uint16_t>(value >> 16));
  const std::size_t nameLength = std::min<std::size_t>(entry.name.size(), 49);
  std::copy_n(entry.name.begin(), nameLength, bytes.begin() + 16);
  return bytes;
}

Reply FakeSpoeServer::Impl::paramFullDesc(uint8_t packetState) {
  const auto withState = [](uint8_t state, uint8_t status) {
    return static_cast<uint16_t>((state << 8) | status);
  };
  if (packetState == kSpoePacketFirst) {
    listedIndexes = indexList();
    nextIndexPosition = 0;
    nextSubindex = 0;
    maxSubindexLatch = 0;
    if (listedIndexes.empty()) {
      return {.status = withState(kSpoePacketLast, kSpoeReplyError), .data = {}};
    }
    // `*p_data_buf = (uint16_t) index_count` stores into a byte pointer, so only the low byte of
    // the count is written. The second byte of the two-byte field is whatever the buffer held.
    return {.status = withState(kSpoePacketMiddle, kSpoeReplyAck),
            .data = {static_cast<uint8_t>(listedIndexes.size()), 0x00}};
  }
  if (packetState != kSpoePacketMiddle) {
    return {.status = withState(kSpoePacketLast, kSpoeReplyError), .data = {}};
  }
  Reply reply{.status = withState(kSpoePacketMiddle, kSpoeReplyAck), .data = {}};
  for (int count = 0; count < 7; ++count) {
    const uint16_t index = listedIndexes[nextIndexPosition];
    const std::vector<uint8_t> entry = encodeEntry(index, nextSubindex);
    reply.data.insert(reply.data.end(), entry.begin(), entry.end());
    if (nextSubindex == 0) {
      maxSubindexLatch = maxSubindex(index);
      if (maxSubindexLatch == 0) {
        ++nextIndexPosition;
      } else {
        ++nextSubindex;
      }
    } else if (nextSubindex < maxSubindexLatch) {
      ++nextSubindex;
    } else {
      ++nextIndexPosition;
      nextSubindex = 0;
    }
    if (nextIndexPosition >= listedIndexes.size()) {
      reply.status = withState(kSpoePacketLast, kSpoeReplyAck);
      break;
    }
  }
  return reply;
}

std::vector<uint8_t> FakeSpoeServer::Impl::fileList() const {
  std::string list;
  for (const auto& [name, content] : files) {
    list += std::format("{}, size: {}\n", name, content.size());
  }
  return {list.begin(), list.end()};
}

Reply FakeSpoeServer::Impl::fileRead(uint8_t packetState, std::span<const uint8_t> data) {
  const auto withState = [](uint8_t state, uint8_t status) {
    return static_cast<uint16_t>((state << 8) | status);
  };
  if (packetState == kSpoePacketFirst) {
    readName.assign(data.begin(), std::find(data.begin(), data.end(), uint8_t{0}));
    readOffset = 0;
    if (readName == "fs-getlist") {
      readContent = fileList();
    } else if (readName.starts_with("fs-remove=")) {
      const std::string message =
          files.erase(readName.substr(10)) > 0 ? "File successfully removed" : "File not found";
      readContent.assign(message.begin(), message.end());
    } else if (const auto it = files.find(readName); it != files.end()) {
      readContent = it->second;
    } else {
      // An empty read, so the driver's check of the file list runs. What the SoC answers for a
      // missing file is not in the firmware source.
      readContent.clear();
    }
    // `AppSockIf_StartReadingFile` only opens the file, so the first reply carries no data.
    return {.status = withState(kSpoePacketMiddle, kSpoeReplyAck), .data = {}};
  }
  if (packetState != kSpoePacketMiddle) {
    return {.status = withState(kSpoePacketLast, 0x00), .data = {}};
  }
  // `READ_BUFFER_SIZE` is 512.
  const std::size_t count = std::min<std::size_t>(512, readContent.size() - readOffset);
  Reply reply{.status = 0,
              .data = {readContent.begin() + static_cast<std::ptrdiff_t>(readOffset),
                       readContent.begin() + static_cast<std::ptrdiff_t>(readOffset + count)}};
  readOffset += count;
  const bool last = readOffset >= readContent.size();
  reply.status = withState(last ? kSpoePacketLast : kSpoePacketMiddle, kSpoeReplyAck);
  return reply;
}

Reply FakeSpoeServer::Impl::fileWrite(uint8_t packetState, std::span<const uint8_t> data) {
  const auto withState = [](uint8_t state, uint8_t status) {
    return static_cast<uint16_t>((state << 8) | status);
  };
  if (packetState == kSpoePacketFirst) {
    writeName.assign(data.begin(), std::find(data.begin(), data.end(), uint8_t{0}));
    writeContent.clear();
    writing = true;
    // In BOOT the COM firmware goes to the netX flash, and the reply carries the return value of
    // `storage_prepare_for_writing`, 0 for success, instead of ACK.
    const bool storage = state == kSpoeStateBoot && writeName == "com_firmware.bin";
    return {.status = withState(kSpoePacketFirst, storage ? 0x00 : kSpoeReplyAck), .data = {}};
  }
  if (!writing) {
    return {.status = withState(kSpoePacketLast, 0x00), .data = {}};
  }
  writeContent.insert(writeContent.end(), data.begin(), data.end());
  if (packetState == kSpoePacketLast) {
    files[writeName] = writeContent;
    writing = false;
  }
  return {.status = withState(packetState, kSpoeReplyAck), .data = {}};
}

std::optional<Reply> FakeSpoeServer::Impl::handle(const Request& request) {
  const std::span<const uint8_t> data = request.data;
  switch (static_cast<SpoeMessage>(request.type)) {
    case SpoeMessage::kSdoRead:
      return readSdo(readU16(data, 0), readU16(data, 2));

    case SpoeMessage::kSdoWrite: {
      if (state == kSpoeStateInit || state == kSpoeStateBoot) {
        return Reply{.status = kSpoeSdoNotAllowedInState, .data = {}};
      }
      const uint16_t index = readU16(data, 0);
      const uint16_t subindex = readU16(data, 2);
      if (const uint16_t status = sdoLookupStatus(index, subindex); status != 0) {
        return Reply{.status = status, .data = {}};
      }
      const std::size_t available = data.size() > 6 ? data.size() - 6 : 0;
      const std::size_t length = std::min<std::size_t>(readU16(data, 4), available);
      const auto value = data.subspan(6, length);
      objects[{index, subindex}].assign(value.begin(), value.end());
      return Reply{};
    }

    case SpoeMessage::kSdoBatchRead: {
      // The firmware decrements its entry count before it checks it. An empty batch wraps that
      // count, so the answer depends on stale bytes in the receive buffer.
      if (data.size() < 4) {
        return std::nullopt;
      }
      Reply reply;
      for (std::size_t offset = 0; offset + 4 <= data.size(); offset += 4) {
        const Reply entry = readSdo(readU16(data, offset), readU16(data, offset + 2));
        if (entry.status != 0) {
          // The firmware stops at the first failure and sends none of the values it read.
          return Reply{.status = entry.status, .data = {}};
        }
        appendU16(reply.data, static_cast<uint16_t>(entry.data.size()));
        reply.data.insert(reply.data.end(), entry.data.begin(), entry.data.end());
      }
      return reply;
    }

    case SpoeMessage::kPdoFrame: {
      if (pdoMode == kSpoePdoModeNone) {
        return Reply{.status = kPdoError, .data = {}};
      }
      // `sqi_copy_socket_cyclicdata_to_soc_frame` accepts outputs in OP only.
      if (pdoMode == kSpoePdoModeControl && state == kSpoeStateOp) {
        lastOutputs = request.data;
      }
      const auto count = static_cast<std::ptrdiff_t>(std::min(inputBuffer.size(), kInputPopLimit));
      Reply reply{.status = 0, .data = {inputBuffer.begin(), inputBuffer.begin() + count}};
      inputBuffer.erase(inputBuffer.begin(), inputBuffer.begin() + count);
      return reply;
    }

    case SpoeMessage::kPdoControl:
      if (data.size() != 1) {
        pdoMode = kSpoePdoModeNone;
        return Reply{.status = kPdoError, .data = {}};
      }
      pdoMode = data[0];
      return Reply{};

    case SpoeMessage::kStateControl: {
      // `AppUtil_ChangeState` returns a bool, so here 1 is success and 0 is failure. Every other
      // status in this protocol uses 0 for success.
      uint16_t status = 0;
      if (!refuseStateChanges) {
        state = data.empty() ? 0 : data[0];
        status = 1;
      }
      return Reply{.status = status, .data = {state}};
    }

    case SpoeMessage::kStateRead:
      return Reply{.status = 0, .data = {state}};

    case SpoeMessage::kServerInfo:
      return Reply{.status = 0,
                   .data = {static_cast<uint8_t>(kSpoeProtocolVersion & 0xFF),
                            static_cast<uint8_t>(kSpoeProtocolVersion >> 8), pdoMode}};

    case SpoeMessage::kDeviceLocate:
      // The status echoes the LED state: 1 when it starts, 0 when it stops.
      locating = !data.empty() && data[0] == 0x01;
      return Reply{.status = static_cast<uint16_t>(locating ? 1 : 0), .data = {}};

    case SpoeMessage::kWatchdogTimeout:
      watchdogTimeoutMs = std::max(readU32(data, 0), kWatchdogMinimumMs);
      return Reply{};

    case SpoeMessage::kParamList: {
      Reply reply;
      for (const uint16_t index : indexList()) {
        appendU16(reply.data, index);
      }
      return reply;
    }

    case SpoeMessage::kParamDesc:
    case SpoeMessage::kParamSubDesc:
      return Reply{.status = 0, .data = encodeEntry(readU16(data, 0), readU16(data, 2))};

    case SpoeMessage::kParamFullDesc:
      return paramFullDesc(static_cast<uint8_t>(request.status & 0xFF));

    case SpoeMessage::kFileRead:
      return fileRead(static_cast<uint8_t>(request.status & 0xFF), data);

    case SpoeMessage::kFileWrite:
      return fileWrite(static_cast<uint8_t>(request.status & 0xFF), data);

    case SpoeMessage::kFirmwareUpdate:
      ++firmwareUpdates;
      return Reply{};
  }
  // The firmware's default case: an empty reply, and the "SPoE active" flag is cleared.
  spoeActive = false;
  return Reply{};
}

FakeSpoeServer::FakeSpoeServer() : impl_(std::make_unique<Impl>()) {
  asio::error_code ec;
  const tcp::endpoint endpoint(asio::ip::make_address_v4("127.0.0.1"), 0);
  if (impl_->acceptor.open(endpoint.protocol(), ec) || impl_->acceptor.bind(endpoint, ec) ||
      impl_->acceptor.listen(asio::socket_base::max_listen_connections, ec)) {
    std::fprintf(stderr, "FakeSpoeServer: cannot listen on 127.0.0.1: %s\n", ec.message().c_str());
    return;
  }
  impl_->port = impl_->acceptor.local_endpoint(ec).port();
  asio::co_spawn(impl_->io, impl_->acceptLoop(), asio::detached);
  impl_->thread = std::thread([this] { impl_->io.run(); });
}

FakeSpoeServer::~FakeSpoeServer() {
  impl_->io.stop();
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
}

uint16_t FakeSpoeServer::port() const { return impl_->port; }

void FakeSpoeServer::setObject(uint16_t index, uint16_t subindex, std::vector<uint8_t> value) {
  const std::scoped_lock lock(impl_->mutex);
  impl_->objects[{index, subindex}] = std::move(value);
}

void FakeSpoeServer::setFile(const std::string& name, std::vector<uint8_t> content) {
  const std::scoped_lock lock(impl_->mutex);
  impl_->files[name] = std::move(content);
}

std::optional<std::vector<uint8_t>> FakeSpoeServer::file(const std::string& name) const {
  const std::scoped_lock lock(impl_->mutex);
  const auto it = impl_->files.find(name);
  if (it == impl_->files.end()) {
    return std::nullopt;
  }
  return it->second;
}

int FakeSpoeServer::firmwareUpdates() const {
  const std::scoped_lock lock(impl_->mutex);
  return impl_->firmwareUpdates;
}

void FakeSpoeServer::setRestartTiming(std::chrono::milliseconds resetDelay,
                                      std::chrono::milliseconds restartDuration) {
  const std::scoped_lock lock(impl_->mutex);
  impl_->resetDelay = resetDelay;
  impl_->restartDuration = restartDuration;
}

void FakeSpoeServer::describeEntry(uint16_t index, uint8_t subindex, FakeSpoeEntry entry) {
  const std::scoped_lock lock(impl_->mutex);
  impl_->descriptions[{index, subindex}] = std::move(entry);
}

std::optional<std::vector<uint8_t>> FakeSpoeServer::object(uint16_t index,
                                                           uint16_t subindex) const {
  const std::scoped_lock lock(impl_->mutex);
  const auto it = impl_->objects.find({index, subindex});
  if (it == impl_->objects.end()) {
    return std::nullopt;
  }
  return it->second;
}

uint8_t FakeSpoeServer::state() const {
  const std::scoped_lock lock(impl_->mutex);
  return impl_->state;
}

void FakeSpoeServer::setState(uint8_t alState) {
  const std::scoped_lock lock(impl_->mutex);
  impl_->state = alState;
}

void FakeSpoeServer::setRefuseStateChanges(bool refuse) {
  const std::scoped_lock lock(impl_->mutex);
  impl_->refuseStateChanges = refuse;
}

uint8_t FakeSpoeServer::pdoMode() const {
  const std::scoped_lock lock(impl_->mutex);
  return impl_->pdoMode;
}

bool FakeSpoeServer::locating() const {
  const std::scoped_lock lock(impl_->mutex);
  return impl_->locating;
}

std::optional<uint32_t> FakeSpoeServer::watchdogTimeoutMs() const {
  const std::scoped_lock lock(impl_->mutex);
  return impl_->watchdogTimeoutMs;
}

bool FakeSpoeServer::spoeActive() const {
  const std::scoped_lock lock(impl_->mutex);
  return impl_->spoeActive;
}

void FakeSpoeServer::pushInputFrame(const std::vector<uint8_t>& frame) {
  const std::scoped_lock lock(impl_->mutex);
  if (impl_->state != kSpoeStateOp) {
    return;
  }
  // `push_to_pdo_fifo_buffer` empties the whole buffer when a frame does not fit, so a client
  // that polls too slowly loses every frame it did not collect, not only the oldest ones.
  if (impl_->inputBuffer.size() + frame.size() > kInputBufferSize) {
    impl_->inputBuffer.clear();
  }
  impl_->inputBuffer.insert(impl_->inputBuffer.end(), frame.begin(), frame.end());
}

std::vector<uint8_t> FakeSpoeServer::lastOutputs() const {
  const std::scoped_lock lock(impl_->mutex);
  return impl_->lastOutputs;
}

void FakeSpoeServer::injectFault(SpoeFault fault) {
  const std::scoped_lock lock(impl_->mutex);
  impl_->nextFault = fault;
  impl_->requestsBeforeFault = 0;
}

void FakeSpoeServer::injectFaultAfter(int skip, SpoeFault fault) {
  const std::scoped_lock lock(impl_->mutex);
  impl_->nextFault = fault;
  impl_->requestsBeforeFault = skip;
}

void FakeSpoeServer::setReplyDelay(std::chrono::milliseconds delay) {
  const std::scoped_lock lock(impl_->mutex);
  impl_->replyDelay = delay;
}

int FakeSpoeServer::connectionCount() const {
  const std::scoped_lock lock(impl_->mutex);
  return impl_->connections;
}

int FakeSpoeServer::ignoredConnectionCount() const {
  const std::scoped_lock lock(impl_->mutex);
  return impl_->ignoredConnections;
}

int FakeSpoeServer::requestCount() const {
  const std::scoped_lock lock(impl_->mutex);
  return impl_->requests;
}

int FakeSpoeServer::unmodelledRequests() const {
  const std::scoped_lock lock(impl_->mutex);
  return impl_->unmodelled;
}

void FakeSpoeServer::dropClient() {
  // The socket belongs to the I/O thread. Close it there and wait, so that the client sees the
  // closed connection before this returns.
  std::promise<void> done;
  asio::post(impl_->io, [this, &done] {
    if (impl_->client) {
      closeSocket(*impl_->client);
      // Free the slot now. The serving coroutine notices the closed socket only later, and a
      // client that reconnects before that would be ignored.
      impl_->client.reset();
    }
    done.set_value();
  });
  done.get_future().wait();
}

}  // namespace mm::comm::testing
