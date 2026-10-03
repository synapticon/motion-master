#include "comm/spoe_connection.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "fake_spoe_server.h"

namespace {

using mm::comm::spoe::ConnectionError;
using mm::comm::spoe::MessageType;
using mm::comm::spoe::SpoeConnection;
using mm::comm::testing::FakeSpoeServer;
using mm::comm::testing::SpoeFault;
using std::chrono::milliseconds;

// The connection against every transport fault the fake can produce. Each fault is one that a real
// drive produces only by accident, so these tests are the only place the handling of each one is
// checked.

constexpr milliseconds kTimeout{1000};
constexpr milliseconds kShortTimeout{100};

std::vector<uint8_t> address(uint16_t index, uint16_t subindex) {
  return {static_cast<uint8_t>(index), static_cast<uint8_t>(index >> 8),
          static_cast<uint8_t>(subindex), static_cast<uint8_t>(subindex >> 8)};
}

void connectTo(SpoeConnection& connection, const FakeSpoeServer& server) {
  const auto connected = connection.connect("127.0.0.1", server.port(), kTimeout);
  ASSERT_TRUE(connected.has_value()) << connected.error();
}

TEST(SpoeConnection, ReturnsTheReplyToARequest) {
  FakeSpoeServer server;
  server.setObject(0x6041, 0, {0x37, 0x02});
  SpoeConnection connection;
  connectTo(connection, server);
  const auto reply = connection.request(MessageType::kSdoRead, 0, address(0x6041, 0), kTimeout);
  ASSERT_TRUE(reply.has_value()) << reply.error().message;
  EXPECT_EQ(reply->status, 0);
  EXPECT_EQ(reply->data, (std::vector<uint8_t>{0x37, 0x02}));
}

TEST(SpoeConnection, FailsToConnectWhereNothingListens) {
  uint16_t port = 0;
  {
    const FakeSpoeServer server;
    port = server.port();
  }
  SpoeConnection connection;
  EXPECT_FALSE(connection.connect("127.0.0.1", port, kTimeout).has_value());
  EXPECT_FALSE(connection.isOpen());
}

TEST(SpoeConnection, RefusesAHostThatIsNotAnIpAddress) {
  SpoeConnection connection;
  EXPECT_FALSE(connection.connect("drive.local", 8080, kTimeout).has_value());
}

TEST(SpoeConnection, JoinsAReplySplitAcrossSegments) {
  FakeSpoeServer server;
  server.setObject(0x6064, 0, {0x01, 0x02, 0x03, 0x04});
  server.injectFault(SpoeFault::kSplitReply);
  SpoeConnection connection;
  connectTo(connection, server);
  const auto reply = connection.request(MessageType::kSdoRead, 0, address(0x6064, 0), kTimeout);
  ASSERT_TRUE(reply.has_value()) << reply.error().message;
  EXPECT_EQ(reply->data, (std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04}));
}

TEST(SpoeConnection, DiscardsALateReplyThatArrivesWithTheNextOne) {
  // The first request times out. Its reply arrives in one segment with the reply to the second
  // request. The second request must get its own reply, not the late one.
  FakeSpoeServer server;
  server.setObject(0x6041, 0, {0x37, 0x02});
  server.setObject(0x6064, 0, {0x01, 0x02, 0x03, 0x04});
  server.injectFault(SpoeFault::kHoldReply);
  SpoeConnection connection;
  connectTo(connection, server);

  const auto first =
      connection.request(MessageType::kSdoRead, 0, address(0x6041, 0), kShortTimeout);
  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(first.error().kind, ConnectionError::Kind::kTimeout);
  EXPECT_TRUE(connection.isOpen());

  const auto second = connection.request(MessageType::kSdoRead, 0, address(0x6064, 0), kTimeout);
  ASSERT_TRUE(second.has_value()) << second.error().message;
  EXPECT_EQ(second->data, (std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04}));
  EXPECT_EQ(connection.discardedReplies(), 1U);
}

TEST(SpoeConnection, KeepsTheFramingWhenAReplyArrivesAfterItsTimeout) {
  // The reply is late, not lost. It is still in the receive buffer when the next request starts,
  // and must be discarded whole rather than misread as the start of the next reply.
  FakeSpoeServer server;
  server.setReplyDelay(milliseconds{200});
  SpoeConnection connection;
  connectTo(connection, server);
  ASSERT_FALSE(connection.request(MessageType::kStateRead, 0, {}, milliseconds{50}).has_value());

  server.setReplyDelay(milliseconds{0});
  const auto reply = connection.request(MessageType::kServerInfo, 0, {}, kTimeout);
  ASSERT_TRUE(reply.has_value()) << reply.error().message;
  EXPECT_EQ(reply->type, static_cast<uint8_t>(MessageType::kServerInfo));
  EXPECT_EQ(reply->data.size(), 3U);
  EXPECT_EQ(connection.discardedReplies(), 1U);
}

TEST(SpoeConnection, TimesOutOnAReplyWithAWrongSequenceId) {
  FakeSpoeServer server;
  server.injectFault(SpoeFault::kWrongSequenceId);
  SpoeConnection connection;
  connectTo(connection, server);
  const auto reply = connection.request(MessageType::kStateRead, 0, {}, kShortTimeout);
  ASSERT_FALSE(reply.has_value());
  EXPECT_EQ(reply.error().kind, ConnectionError::Kind::kTimeout);
  EXPECT_EQ(connection.discardedReplies(), 1U);
  EXPECT_TRUE(connection.request(MessageType::kStateRead, 0, {}, kTimeout).has_value());
}

TEST(SpoeConnection, ClosesOnAHeaderThatClaimsTooMuchData) {
  FakeSpoeServer server;
  server.injectFault(SpoeFault::kOversizeLength);
  SpoeConnection connection;
  connectTo(connection, server);
  const auto reply = connection.request(MessageType::kStateRead, 0, {}, kTimeout);
  ASSERT_FALSE(reply.has_value());
  EXPECT_EQ(reply.error().kind, ConnectionError::Kind::kClosed);
  EXPECT_FALSE(connection.isOpen());
}

TEST(SpoeConnection, TimesOutOnSilenceAndStaysUsable) {
  FakeSpoeServer server;
  server.injectFault(SpoeFault::kNoReply);
  SpoeConnection connection;
  connectTo(connection, server);
  const auto silent = connection.request(MessageType::kStateRead, 0, {}, kShortTimeout);
  ASSERT_FALSE(silent.has_value());
  EXPECT_EQ(silent.error().kind, ConnectionError::Kind::kTimeout);
  EXPECT_TRUE(connection.request(MessageType::kStateRead, 0, {}, kTimeout).has_value());
}

TEST(SpoeConnection, ReportsAConnectionTheDriveClosed) {
  FakeSpoeServer server;
  server.injectFault(SpoeFault::kDropConnection);
  SpoeConnection connection;
  connectTo(connection, server);
  const auto reply = connection.request(MessageType::kStateRead, 0, {}, kTimeout);
  ASSERT_FALSE(reply.has_value());
  EXPECT_EQ(reply.error().kind, ConnectionError::Kind::kClosed);
  EXPECT_FALSE(connection.isOpen());

  const auto after = connection.request(MessageType::kStateRead, 0, {}, kTimeout);
  ASSERT_FALSE(after.has_value());
  EXPECT_EQ(after.error().kind, ConnectionError::Kind::kClosed);
}

TEST(SpoeConnection, ReconnectsAfterTheDriveDropsIt) {
  FakeSpoeServer server;
  SpoeConnection connection;
  connectTo(connection, server);
  // A reply proves that the fake accepted the connection. Without one, dropClient can run before
  // the accept, find no client, and close nothing.
  ASSERT_TRUE(connection.request(MessageType::kStateRead, 0, {}, kTimeout).has_value());
  server.dropClient();
  EXPECT_FALSE(connection.request(MessageType::kStateRead, 0, {}, kTimeout).has_value());
  connectTo(connection, server);
  EXPECT_TRUE(connection.request(MessageType::kStateRead, 0, {}, kTimeout).has_value());
}

TEST(SpoeConnection, RefusesTooMuchDataWithoutSendingIt) {
  FakeSpoeServer server;
  SpoeConnection connection;
  connectTo(connection, server);
  const std::vector<uint8_t> data(mm::comm::spoe::kMaxDataSize + 1);
  const auto reply = connection.request(MessageType::kSdoWrite, 0, data, kTimeout);
  ASSERT_FALSE(reply.has_value());
  EXPECT_EQ(reply.error().kind, ConnectionError::Kind::kInvalidRequest);
  EXPECT_TRUE(connection.isOpen());
  EXPECT_EQ(server.requestCount(), 0);
}

TEST(SpoeConnection, AnswersEveryRequestFromSeveralThreads) {
  // The control plane and the process-data exchange share one connection. Their requests must
  // take turns, and each must get its own reply.
  FakeSpoeServer server;
  server.setObject(0x6041, 0, {0x37, 0x02});
  SpoeConnection connection;
  connectTo(connection, server);
  constexpr int kThreads = 4;
  constexpr int kRequests = 50;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  std::atomic<int> failures{0};
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < kRequests; ++i) {
        const auto reply =
            connection.request(MessageType::kSdoRead, 0, address(0x6041, 0), kTimeout);
        if (!reply || reply->data != std::vector<uint8_t>{0x37, 0x02}) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(server.requestCount(), kThreads * kRequests);
  EXPECT_EQ(connection.discardedReplies(), 0U);
}

}  // namespace
