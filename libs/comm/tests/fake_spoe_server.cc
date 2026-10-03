#include "fake_spoe_server.h"

#include <algorithm>
#include <array>
#include <asio.hpp>
#include <chrono>
#include <cstdio>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <span>
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
  uint8_t state = kSpoeStatePreOp;
  bool refuseStateChanges = false;
  uint8_t pdoMode = kSpoePdoModeNone;
  bool locating = false;
  std::optional<uint32_t> watchdogTimeoutMs;
  bool spoeActive = true;
  std::vector<uint8_t> inputBuffer;
  std::vector<uint8_t> lastOutputs;
  std::optional<SpoeFault> nextFault;
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
  uint16_t sdoLookupStatus(uint16_t index, uint16_t subindex) const;
};

asio::awaitable<void> FakeSpoeServer::Impl::acceptLoop() {
  for (;;) {
    auto [ec, socket] = co_await acceptor.async_accept(kNoThrow);
    if (ec) {
      co_return;
    }
    asio::error_code ignored;
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
      fault = std::exchange(nextFault, std::nullopt);
      delay = replyDelay;
      reply = handle(request);
      if (!reply) {
        ++unmodelled;
      }
    }
    if (!reply) {
      break;
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

    case SpoeMessage::kFirmwareUpdate:
    case SpoeMessage::kFileRead:
    case SpoeMessage::kFileWrite:
    case SpoeMessage::kParamList:
    case SpoeMessage::kParamDesc:
    case SpoeMessage::kParamSubDesc:
    case SpoeMessage::kParamFullDesc:
      return std::nullopt;
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
