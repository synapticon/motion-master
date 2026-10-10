#pragma once

// One TCP connection to one SPoE server, the netX of one drive.
//
// The drive answers one request at a time, so requests on one connection take turns behind a
// mutex. The control plane and the process-data exchange share a connection that way.
//
// A reply is matched to its request by sequence id and type. A reply that does not match is a late
// answer to an earlier request that timed out. It is logged and discarded, so it is never returned
// for a later request. Received bytes stay in a buffer between requests, so a reply that arrives
// after its request timed out does not break the framing of the next one.
//
// Asio stays inside the implementation file, so this header does not include it.

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

#include "comm/spoe_frame.h"

namespace mm::comm::spoe {

/// Why a request got no reply. The driver branches on it: a closed connection means the device is
/// lost, and a timeout leaves the connection usable.
struct ConnectionError {
  enum class Kind : uint8_t {
    /// No matching reply arrived in time. The connection stays open.
    kTimeout,
    /// The connection is closed: by the drive, by a transport error, or because a reply broke the
    /// protocol. Every later request fails the same way until @c connect succeeds again.
    kClosed,
    /// The request could not be encoded, so nothing was sent. The connection stays open.
    kInvalidRequest,
  };

  Kind kind = Kind::kClosed;
  std::string message;
};

class SpoeConnection {
 public:
  SpoeConnection();
  ~SpoeConnection();

  SpoeConnection(const SpoeConnection&) = delete;
  SpoeConnection& operator=(const SpoeConnection&) = delete;

  /// Opens the connection. An open connection is closed first.
  std::expected<void, std::string> connect(const std::string& host, uint16_t port,
                                           std::chrono::milliseconds timeout);

  /// Closes the connection. Waits for a request in progress to finish first.
  void close();

  bool isOpen() const;

  /// Sends one request and waits up to @p timeout for its reply.
  std::expected<Frame, ConnectionError> request(MessageType type, uint16_t status,
                                                std::span<const uint8_t> data,
                                                std::chrono::milliseconds timeout);

  /// The replies discarded because they matched no request in progress.
  uint64_t discardedReplies() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mm::comm::spoe
