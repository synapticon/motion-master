#include "comm/spoe_connection.h"

#include <spdlog/spdlog.h>

#include <array>
#include <asio.hpp>
#include <atomic>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mm::comm::spoe {

namespace {

using asio::ip::tcp;
using Clock = std::chrono::steady_clock;

struct OperationResult {
  asio::error_code error;
  std::size_t bytes = 0;
};

// Runs one asynchronous operation until it completes or @p deadline passes. Returns nullopt on the
// deadline, after the operation is cancelled. An operation that completes while it is cancelled
// still returns its result, so bytes that arrived at the deadline are not lost.
template <typename Start>
std::optional<OperationResult> runUntil(asio::io_context& io, tcp::socket& socket,
                                        Clock::time_point deadline, Start start) {
  std::optional<OperationResult> result;
  start([&result](const asio::error_code& error, std::size_t bytes) {
    result = OperationResult{.error = error, .bytes = bytes};
  });
  io.restart();
  io.run_until(deadline);
  if (result) {
    return result;
  }
  asio::error_code ignored;
  (void)socket.cancel(ignored);
  io.restart();
  io.run();
  if (result && result->error != asio::error::operation_aborted) {
    return result;
  }
  return std::nullopt;
}

}  // namespace

struct SpoeConnection::Impl {
  // Declared first, so that it is destroyed last: the socket needs its services.
  asio::io_context io;
  tcp::socket socket{io};

  std::mutex mutex;
  std::atomic<bool> open{false};
  std::atomic<uint64_t> discarded{0};
  std::string peer;
  uint16_t nextSequenceId = 1;
  std::vector<uint8_t> received;

  void closeLocked() {
    asio::error_code ignored;
    (void)socket.shutdown(tcp::socket::shutdown_both, ignored);
    (void)socket.close(ignored);
    received.clear();
    open.store(false);
  }

  ConnectionError closedError(std::string message) {
    closeLocked();
    return ConnectionError{.kind = ConnectionError::Kind::kClosed,
                           .message = std::format("SPoE {}: {}", peer, message)};
  }

  // Takes the first whole frame out of `received`. Returns nullopt while the frame is incomplete,
  // and an error when its header claims more data than a frame can carry. The firmware never sends
  // such a header, so the stream is no longer trusted and the connection is closed.
  std::expected<std::optional<Frame>, std::string> takeFrame() {
    const auto header = decodeHeader(received);
    if (!header) {
      return std::nullopt;
    }
    if (header->dataLength > kMaxDataSize) {
      return std::unexpected(std::format("reply claims {} data bytes, more than the {}-byte limit",
                                         header->dataLength, kMaxDataSize));
    }
    const std::size_t size = kHeaderSize + header->dataLength;
    if (received.size() < size) {
      return std::nullopt;
    }
    Frame frame{.type = header->type,
                .sequenceId = header->sequenceId,
                .status = header->status,
                .data = {received.begin() + static_cast<std::ptrdiff_t>(kHeaderSize),
                         received.begin() + static_cast<std::ptrdiff_t>(size)}};
    received.erase(received.begin(), received.begin() + static_cast<std::ptrdiff_t>(size));
    return frame;
  }
};

SpoeConnection::SpoeConnection() : impl_(std::make_unique<Impl>()) {}

SpoeConnection::~SpoeConnection() { close(); }

std::expected<void, std::string> SpoeConnection::connect(const std::string& host, uint16_t port,
                                                         std::chrono::milliseconds timeout) {
  const std::scoped_lock lock(impl_->mutex);
  impl_->closeLocked();
  impl_->peer = std::format("{}:{}", host, port);

  asio::error_code error;
  const asio::ip::address address = asio::ip::make_address(host, error);
  if (error) {
    return std::unexpected(std::format("SPoE {}: not an IP address", impl_->peer));
  }
  const tcp::endpoint endpoint(address, port);
  const auto result =
      runUntil(impl_->io, impl_->socket, Clock::now() + timeout, [this, &endpoint](auto handler) {
        impl_->socket.async_connect(
            endpoint, [handler](const asio::error_code& e) mutable { handler(e, 0); });
      });
  if (!result) {
    impl_->closeLocked();
    return std::unexpected(
        std::format("SPoE {}: no connection within {} ms", impl_->peer, timeout.count()));
  }
  if (result->error) {
    impl_->closeLocked();
    return std::unexpected(std::format("SPoE {}: {}", impl_->peer, result->error.message()));
  }
  // The firmware reads one TCP receive as one request, so a request must leave in one segment
  // and leave at once. Nagle's algorithm would hold it back.
  (void)impl_->socket.set_option(tcp::no_delay(true), error);
  impl_->open.store(true);
  return {};
}

void SpoeConnection::close() {
  const std::scoped_lock lock(impl_->mutex);
  impl_->closeLocked();
}

bool SpoeConnection::isOpen() const { return impl_->open.load(); }

uint64_t SpoeConnection::discardedReplies() const { return impl_->discarded.load(); }

std::expected<Frame, ConnectionError> SpoeConnection::request(MessageType type, uint16_t status,
                                                              std::span<const uint8_t> data,
                                                              std::chrono::milliseconds timeout) {
  const std::scoped_lock lock(impl_->mutex);
  Impl& impl = *impl_;
  if (!impl.open.load()) {
    return std::unexpected(
        ConnectionError{.kind = ConnectionError::Kind::kClosed,
                        .message = std::format("SPoE {}: not connected", impl.peer)});
  }

  const uint16_t sequenceId = impl.nextSequenceId++;
  const auto bytes = encodeFrame(Frame{.type = static_cast<uint8_t>(type),
                                       .sequenceId = sequenceId,
                                       .status = status,
                                       .data = {data.begin(), data.end()}});
  if (!bytes) {
    return std::unexpected(
        ConnectionError{.kind = ConnectionError::Kind::kInvalidRequest,
                        .message = std::format("SPoE {}: {}", impl.peer, bytes.error())});
  }

  const Clock::time_point deadline = Clock::now() + timeout;
  const auto written = runUntil(impl.io, impl.socket, deadline, [&impl, &bytes](auto handler) {
    asio::async_write(impl.socket, asio::buffer(*bytes), handler);
  });
  if (!written) {
    // Part of the request may be on the wire, so the drive would read the next request from
    // the middle of this one.
    return std::unexpected(impl.closedError("the request could not be sent in time"));
  }
  if (written->error) {
    return std::unexpected(impl.closedError(written->error.message()));
  }

  for (;;) {
    auto frame = impl.takeFrame();
    if (!frame) {
      return std::unexpected(impl.closedError(frame.error()));
    }
    if (*frame) {
      const Frame& reply = **frame;
      if (reply.sequenceId == sequenceId && reply.type == static_cast<uint8_t>(type)) {
        return std::move(**frame);
      }
      impl.discarded.fetch_add(1);
      spdlog::warn(
          "SPoE {}: discarded a reply of type 0x{:02X} with sequence id {} while waiting for type "
          "0x{:02X} with sequence id {}",
          impl.peer, reply.type, reply.sequenceId, static_cast<uint8_t>(type), sequenceId);
      continue;
    }

    std::array<uint8_t, 1024> chunk{};
    const auto read = runUntil(impl.io, impl.socket, deadline, [&impl, &chunk](auto handler) {
      impl.socket.async_read_some(asio::buffer(chunk), handler);
    });
    if (!read) {
      return std::unexpected(ConnectionError{
          .kind = ConnectionError::Kind::kTimeout,
          .message = std::format("SPoE {}: no reply to message type 0x{:02X} within {} ms",
                                 impl.peer, static_cast<uint8_t>(type), timeout.count())});
    }
    if (read->error) {
      const bool closedByDrive =
          read->error == asio::error::eof || read->error == asio::error::connection_reset;
      return std::unexpected(impl.closedError(closedByDrive ? "the drive closed the connection"
                                                            : read->error.message()));
    }
    impl.received.insert(impl.received.end(), chunk.begin(),
                         chunk.begin() + static_cast<std::ptrdiff_t>(read->bytes));
  }
}

}  // namespace mm::comm::spoe
