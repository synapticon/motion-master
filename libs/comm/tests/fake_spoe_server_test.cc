#include "fake_spoe_server.h"

#include <gtest/gtest.h>

#include <array>
#include <asio.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using asio::ip::tcp;
using mm::comm::testing::FakeSpoeServer;
using mm::comm::testing::kSpoeHeaderSize;
using mm::comm::testing::kSpoeMaxDataSize;
using mm::comm::testing::SpoeFault;
using mm::comm::testing::SpoeMessage;
using std::chrono::milliseconds;

// These tests pin the fake against the firmware source. The SPoE driver tests trust the fake, so a
// wrong answer here becomes a wrong driver later. Each test names the firmware behaviour it copies.

struct Frame {
  uint8_t type = 0;
  uint16_t sequenceId = 0;
  uint16_t status = 0;
  uint16_t length = 0;
  std::vector<uint8_t> data;
};

uint16_t u16At(const std::vector<uint8_t>& bytes, std::size_t offset) {
  return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

// A blocking client with a timeout on every read. It frames replies the simple way, by header and
// then data length, which is enough to check the fake.
class Client {
 public:
  explicit Client(uint16_t port) {
    asio::error_code ec;
    EXPECT_FALSE(socket_.connect(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), port), ec))
        << ec.message();
  }

  void send(SpoeMessage type, uint16_t sequenceId, const std::vector<uint8_t>& data = {},
            uint16_t status = 0) {
    sendRaw(static_cast<uint8_t>(type), sequenceId, data, status);
  }

  void sendRaw(uint8_t type, uint16_t sequenceId, const std::vector<uint8_t>& data,
               uint16_t status = 0) {
    std::vector<uint8_t> frame{type,
                               static_cast<uint8_t>(sequenceId),
                               static_cast<uint8_t>(sequenceId >> 8),
                               static_cast<uint8_t>(status),
                               static_cast<uint8_t>(status >> 8),
                               static_cast<uint8_t>(data.size()),
                               static_cast<uint8_t>(data.size() >> 8)};
    frame.insert(frame.end(), data.begin(), data.end());
    asio::error_code ec;
    asio::write(socket_, asio::buffer(frame), ec);
    EXPECT_FALSE(ec) << ec.message();
  }

  // Exactly @p count bytes, or nullopt when they do not arrive in time or the connection closes.
  std::optional<std::vector<uint8_t>> read(std::size_t count,
                                           milliseconds timeout = milliseconds{1000}) {
    std::vector<uint8_t> bytes(count);
    asio::error_code result = asio::error::would_block;
    asio::async_read(socket_, asio::buffer(bytes),
                     [&](const asio::error_code& ec, std::size_t) { result = ec; });
    io_.restart();
    io_.run_for(timeout);
    if (result == asio::error::would_block) {
      asio::error_code ignored;
      (void)socket_.cancel(ignored);
      io_.restart();
      io_.run();
      return std::nullopt;
    }
    if (result) {
      return std::nullopt;
    }
    return bytes;
  }

  std::optional<Frame> receive(milliseconds timeout = milliseconds{1000}) {
    const auto header = read(kSpoeHeaderSize, timeout);
    if (!header) {
      return std::nullopt;
    }
    Frame frame{.type = (*header)[0],
                .sequenceId = u16At(*header, 1),
                .status = u16At(*header, 3),
                .length = u16At(*header, 5),
                .data = {}};
    if (frame.length > 0 && frame.length <= kSpoeMaxDataSize) {
      const auto data = read(frame.length, timeout);
      if (!data) {
        return std::nullopt;
      }
      frame.data = *data;
    }
    return frame;
  }

  Frame request(SpoeMessage type, uint16_t sequenceId, const std::vector<uint8_t>& data = {},
                uint16_t status = 0) {
    send(type, sequenceId, data, status);
    const auto frame = receive();
    EXPECT_TRUE(frame.has_value());
    return frame.value_or(Frame{});
  }

  // True when the server closes the connection within @p timeout.
  bool closed(milliseconds timeout = milliseconds{1000}) {
    std::array<uint8_t, 1> byte{};
    asio::error_code result = asio::error::would_block;
    socket_.async_read_some(asio::buffer(byte),
                            [&](const asio::error_code& ec, std::size_t) { result = ec; });
    io_.restart();
    io_.run_for(timeout);
    if (result == asio::error::would_block) {
      asio::error_code ignored;
      (void)socket_.cancel(ignored);
      io_.restart();
      io_.run();
      return false;
    }
    return result == asio::error::eof || result == asio::error::connection_reset;
  }

 private:
  asio::io_context io_;
  tcp::socket socket_{io_};
};

std::vector<uint8_t> address(uint16_t index, uint16_t subindex) {
  return {static_cast<uint8_t>(index), static_cast<uint8_t>(index >> 8),
          static_cast<uint8_t>(subindex), static_cast<uint8_t>(subindex >> 8)};
}

TEST(FakeSpoeServer, ServerInfoReportsTheVersionLittleEndianAndThePdoMode) {
  // `SERVER_INFO` writes 0x0102 low byte first. A client that reads it big-endian sees 0x0201.
  FakeSpoeServer server;
  Client client(server.port());
  const Frame frame = client.request(SpoeMessage::kServerInfo, 7);
  EXPECT_EQ(frame.type, static_cast<uint8_t>(SpoeMessage::kServerInfo));
  EXPECT_EQ(frame.sequenceId, 7);
  EXPECT_EQ(frame.status, 0);
  EXPECT_EQ(frame.data, (std::vector<uint8_t>{0x02, 0x01, mm::comm::testing::kSpoePdoModeNone}));
}

TEST(FakeSpoeServer, ProtocolVersion100ReportsNoPdoModeAndNoWatchdog) {
  FakeSpoeServer server;
  server.setProtocolVersion(mm::comm::testing::kSpoeProtocolVersion100);
  Client client(server.port());
  EXPECT_EQ(client.request(SpoeMessage::kServerInfo, 1).data, (std::vector<uint8_t>{0x00, 0x01}));
  client.request(SpoeMessage::kWatchdogTimeout, 2, {75, 0, 0, 0});
  EXPECT_FALSE(server.watchdogTimeoutMs().has_value());
  EXPECT_FALSE(server.spoeActive());
}

TEST(FakeSpoeServer, SdoReadAnswersTheValueOrTheFirmwareError) {
  FakeSpoeServer server;
  server.setObject(0x6041, 0, {0x37, 0x02});
  server.setObject(0x1018, 1, {0xD2, 0x22, 0x00, 0x00});
  Client client(server.port());

  Frame frame = client.request(SpoeMessage::kSdoRead, 1, address(0x6041, 0));
  EXPECT_EQ(frame.status, 0);
  EXPECT_EQ(frame.data, (std::vector<uint8_t>{0x37, 0x02}));

  // `find_entry`: an unknown index is 2, a known index with an unknown subindex is 13.
  EXPECT_EQ(client.request(SpoeMessage::kSdoRead, 2, address(0x6042, 0)).status,
            mm::comm::testing::kSpoeSdoNotFound);
  EXPECT_EQ(client.request(SpoeMessage::kSdoRead, 3, address(0x1018, 9)).status,
            mm::comm::testing::kSpoeSdoSubNotFound);
}

TEST(FakeSpoeServer, SdoAccessIsRefusedInInitAndBoot) {
  // `AppUtil_GetParameter` and `AppUtil_SetParameter` return 0xFFFF before they look anything up.
  FakeSpoeServer server;
  server.setObject(0x6041, 0, {0x37, 0x02});
  Client client(server.port());
  for (const uint8_t state :
       {mm::comm::testing::kSpoeStateInit, mm::comm::testing::kSpoeStateBoot}) {
    server.setState(state);
    EXPECT_EQ(client.request(SpoeMessage::kSdoRead, 1, address(0x6041, 0)).status,
              mm::comm::testing::kSpoeSdoNotAllowedInState);
  }
}

TEST(FakeSpoeServer, SdoWriteStoresTheValue) {
  FakeSpoeServer server;
  server.setObject(0x6040, 0, {0x00, 0x00});
  Client client(server.port());
  std::vector<uint8_t> request = address(0x6040, 0);
  request.insert(request.end(), {0x02, 0x00, 0x0F, 0x00});
  EXPECT_EQ(client.request(SpoeMessage::kSdoWrite, 1, request).status, 0);
  EXPECT_EQ(server.object(0x6040, 0), (std::vector<uint8_t>{0x0F, 0x00}));
}

TEST(FakeSpoeServer, BatchReadPrefixesEachValueWithItsSize) {
  FakeSpoeServer server;
  server.setObject(0x6041, 0, {0x37, 0x02});
  server.setObject(0x6064, 0, {0x01, 0x02, 0x03, 0x04});
  Client client(server.port());
  std::vector<uint8_t> request = address(0x6041, 0);
  const std::vector<uint8_t> second = address(0x6064, 0);
  request.insert(request.end(), second.begin(), second.end());
  const Frame frame = client.request(SpoeMessage::kSdoBatchRead, 1, request);
  EXPECT_EQ(frame.status, 0);
  EXPECT_EQ(frame.data,
            (std::vector<uint8_t>{0x02, 0x00, 0x37, 0x02, 0x04, 0x00, 0x01, 0x02, 0x03, 0x04}));
}

TEST(FakeSpoeServer, BatchReadSendsNoValueWhenOneEntryFails) {
  // `AppSockIf_GetParameterBatch` stops at the first failure. The values it read before that are
  // not sent, so a client cannot keep a partial batch.
  FakeSpoeServer server;
  server.setObject(0x6041, 0, {0x37, 0x02});
  Client client(server.port());
  std::vector<uint8_t> request = address(0x6041, 0);
  const std::vector<uint8_t> missing = address(0x6042, 0);
  request.insert(request.end(), missing.begin(), missing.end());
  const Frame frame = client.request(SpoeMessage::kSdoBatchRead, 1, request);
  EXPECT_EQ(frame.status, mm::comm::testing::kSpoeSdoNotFound);
  EXPECT_TRUE(frame.data.empty());
}

TEST(FakeSpoeServer, StateControlReportsSuccessAsOneAndTheStateInTheData) {
  FakeSpoeServer server;
  Client client(server.port());
  Frame frame = client.request(SpoeMessage::kStateControl, 1, {mm::comm::testing::kSpoeStateOp});
  EXPECT_EQ(frame.status, 1);
  EXPECT_EQ(frame.data, (std::vector<uint8_t>{mm::comm::testing::kSpoeStateOp}));

  server.setRefuseStateChanges(true);
  frame = client.request(SpoeMessage::kStateControl, 2, {mm::comm::testing::kSpoeStatePreOp});
  EXPECT_EQ(frame.status, 0);
  EXPECT_EQ(frame.data, (std::vector<uint8_t>{mm::comm::testing::kSpoeStateOp}));

  frame = client.request(SpoeMessage::kStateRead, 3);
  EXPECT_EQ(frame.status, 0);
  EXPECT_EQ(frame.data, (std::vector<uint8_t>{mm::comm::testing::kSpoeStateOp}));
}

TEST(FakeSpoeServer, PdoFrameFailsWithNoPdoMode) {
  FakeSpoeServer server;
  Client client(server.port());
  EXPECT_EQ(client.request(SpoeMessage::kPdoFrame, 1).status, 1);
}

TEST(FakeSpoeServer, PdoControlWithWrongLengthResetsTheMode) {
  FakeSpoeServer server;
  Client client(server.port());
  EXPECT_EQ(
      client.request(SpoeMessage::kPdoControl, 1, {mm::comm::testing::kSpoePdoModeMonitor}).status,
      0);
  EXPECT_EQ(server.pdoMode(), mm::comm::testing::kSpoePdoModeMonitor);
  EXPECT_EQ(client.request(SpoeMessage::kPdoControl, 2, {0x01, 0x00}).status, 1);
  EXPECT_EQ(server.pdoMode(), mm::comm::testing::kSpoePdoModeNone);
}

TEST(FakeSpoeServer, PdoFrameReturnsTheBufferedFramesConcatenated) {
  // `pop_from_pdo_fifo_buffer` returns the buffer as one run of bytes, with nothing between the
  // frames. The client splits it by the input frame size.
  FakeSpoeServer server;
  server.setState(mm::comm::testing::kSpoeStateOp);
  server.pushInputFrame({0x01, 0x02, 0x03});
  server.pushInputFrame({0x04, 0x05, 0x06});
  Client client(server.port());
  client.request(SpoeMessage::kPdoControl, 1, {mm::comm::testing::kSpoePdoModeMonitor});
  Frame frame = client.request(SpoeMessage::kPdoFrame, 2);
  EXPECT_EQ(frame.status, 0);
  EXPECT_EQ(frame.data, (std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04, 0x05, 0x06}));
  EXPECT_TRUE(client.request(SpoeMessage::kPdoFrame, 3).data.empty());
}

TEST(FakeSpoeServer, PdoFrameReturnsAtMost500BytesAndCanCutAFrame) {
  // 500 is not a multiple of 24, so the first reply ends in the middle of the twenty-first frame.
  // The client must keep that part and join it to the start of the next reply.
  FakeSpoeServer server;
  server.setState(mm::comm::testing::kSpoeStateOp);
  for (uint8_t i = 0; i < 21; ++i) {
    server.pushInputFrame(std::vector<uint8_t>(24, i));
  }
  Client client(server.port());
  client.request(SpoeMessage::kPdoControl, 1, {mm::comm::testing::kSpoePdoModeMonitor});
  EXPECT_EQ(client.request(SpoeMessage::kPdoFrame, 2).data.size(), 500U);
  const Frame rest = client.request(SpoeMessage::kPdoFrame, 3);
  EXPECT_EQ(rest.data, std::vector<uint8_t>(4, 20));
}

TEST(FakeSpoeServer, AFullInputBufferIsEmptiedBeforeTheNextFrame) {
  // `push_to_pdo_fifo_buffer` resets its write index when a frame does not fit. Every frame the
  // client did not collect is lost, and the buffer then holds only the newest frame.
  FakeSpoeServer server;
  server.setState(mm::comm::testing::kSpoeStateOp);
  for (uint8_t i = 0; i < 22; ++i) {
    server.pushInputFrame(std::vector<uint8_t>(24, i));
  }
  Client client(server.port());
  client.request(SpoeMessage::kPdoControl, 1, {mm::comm::testing::kSpoePdoModeMonitor});
  EXPECT_EQ(client.request(SpoeMessage::kPdoFrame, 2).data, std::vector<uint8_t>(24, 21));
}

TEST(FakeSpoeServer, TheInputBufferFillsInOpOnly) {
  // `sqi_cyclic_rx_handler` pushes a frame only in OP. In SAFE-OP a Monitor-mode client gets
  // empty replies.
  FakeSpoeServer server;
  server.setState(mm::comm::testing::kSpoeStateSafeOp);
  server.pushInputFrame({0x01, 0x02, 0x03});
  Client client(server.port());
  client.request(SpoeMessage::kPdoControl, 1, {mm::comm::testing::kSpoePdoModeMonitor});
  const Frame frame = client.request(SpoeMessage::kPdoFrame, 2);
  EXPECT_EQ(frame.status, 0);
  EXPECT_TRUE(frame.data.empty());
}

TEST(FakeSpoeServer, ControlModeAcceptsOutputsInOpOnly) {
  FakeSpoeServer server;
  Client client(server.port());
  client.request(SpoeMessage::kPdoControl, 1, {mm::comm::testing::kSpoePdoModeControl});
  client.request(SpoeMessage::kPdoFrame, 2, {0xAA, 0xBB});
  EXPECT_TRUE(server.lastOutputs().empty());
  server.setState(mm::comm::testing::kSpoeStateOp);
  client.request(SpoeMessage::kPdoFrame, 3, {0xAA, 0xBB});
  EXPECT_EQ(server.lastOutputs(), (std::vector<uint8_t>{0xAA, 0xBB}));
}

TEST(FakeSpoeServer, DeviceLocateEchoesTheLedState) {
  FakeSpoeServer server;
  Client client(server.port());
  EXPECT_EQ(client.request(SpoeMessage::kDeviceLocate, 1, {0x01}).status, 1);
  EXPECT_TRUE(server.locating());
  EXPECT_EQ(client.request(SpoeMessage::kDeviceLocate, 2, {0x00}).status, 0);
  EXPECT_FALSE(server.locating());
}

TEST(FakeSpoeServer, WatchdogTimeoutIsRaisedToTheMinimum) {
  FakeSpoeServer server;
  Client client(server.port());
  client.request(SpoeMessage::kWatchdogTimeout, 1, {20, 0, 0, 0});
  EXPECT_EQ(server.watchdogTimeoutMs(), 50U);
  client.request(SpoeMessage::kWatchdogTimeout, 2, {75, 0, 0, 0});
  EXPECT_EQ(server.watchdogTimeoutMs(), 75U);
}

TEST(FakeSpoeServer, AnUnknownTypeGetsAnEmptyReplyAndClearsSpoeActive) {
  // 0x05 is `PDO_MAP` in the firmware enum, and no case handles it.
  FakeSpoeServer server;
  Client client(server.port());
  client.sendRaw(0x05, 1, {});
  const auto frame = client.receive();
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->status, 0);
  EXPECT_TRUE(frame->data.empty());
  EXPECT_FALSE(server.spoeActive());
}

TEST(FakeSpoeServer, AnUnmodelledRequestClosesTheConnection) {
  // An empty batch read: the firmware decrements its count past zero and reads stale bytes, so
  // there is no answer to model.
  FakeSpoeServer server;
  Client client(server.port());
  client.send(SpoeMessage::kSdoBatchRead, 1, {});
  EXPECT_TRUE(client.closed());
  EXPECT_EQ(server.unmodelledRequests(), 1);
}

// A small dictionary: one VAR object and one RECORD with two entries.
void describeSmallDictionary(FakeSpoeServer& server) {
  server.describeEntry(0x1000, 0,
                       {.dataType = 0x0007,
                        .objectCode = 0x07,
                        .bitLength = 32,
                        .access = 0x07,
                        .name = "Device type"});
  server.describeEntry(
      0x1018, 0,
      {.dataType = 0x0005, .objectCode = 0x09, .bitLength = 8, .access = 0x07, .name = "Identity"});
  server.describeEntry(0x1018, 1,
                       {.dataType = 0x0007,
                        .objectCode = 0x07,
                        .bitLength = 32,
                        .access = 0x07,
                        .name = "Vendor ID"});
  server.describeEntry(0x1018, 2,
                       {.dataType = 0x0007,
                        .objectCode = 0x07,
                        .bitLength = 32,
                        .access = 0x07,
                        .name = "Product code"});
}

const mm::comm::testing::FakeSpoeEntry kByteEntry{
    .dataType = 0x0005, .objectCode = 0x07, .bitLength = 8, .access = 0x07, .name = "Byte"};

uint8_t packetState(const Frame& frame) { return static_cast<uint8_t>(frame.status >> 8); }

TEST(FakeSpoeServer, IndexListHoldsEveryObjectOnce) {
  FakeSpoeServer server;
  describeSmallDictionary(server);
  Client client(server.port());
  EXPECT_EQ(client.request(SpoeMessage::kParamList, 1).data,
            (std::vector<uint8_t>{0x00, 0x10, 0x18, 0x10}));
}

TEST(FakeSpoeServer, IndexListStopsAtTwoHundredObjects) {
  // `sdoinfo_get_list` is called with `MAX_INDEX_LIST`, which is 200.
  FakeSpoeServer server;
  for (uint16_t i = 0; i < 250; ++i) {
    server.describeEntry(static_cast<uint16_t>(0x2000 + i), 0, kByteEntry);
  }
  Client client(server.port());
  EXPECT_EQ(client.request(SpoeMessage::kParamList, 1).data.size(), 400U);
}

TEST(FakeSpoeServer, ParameterListSendsTheCountThenEntriesInPacketsOfSeven) {
  FakeSpoeServer server;
  describeSmallDictionary(server);
  Client client(server.port());

  // The first packet carries only the object count, as one byte of a two-byte field.
  const Frame first =
      client.request(SpoeMessage::kParamFullDesc, 1, {}, mm::comm::testing::kSpoePacketFirst);
  EXPECT_EQ(packetState(first), mm::comm::testing::kSpoePacketMiddle);
  ASSERT_EQ(first.data.size(), 2U);
  EXPECT_EQ(first.data[0], 2);

  // Four entries in all: 0x1000:00, 0x1018:00, 0x1018:01 and 0x1018:02. They fit one packet.
  const Frame middle =
      client.request(SpoeMessage::kParamFullDesc, 2, {}, mm::comm::testing::kSpoePacketMiddle);
  EXPECT_EQ(packetState(middle), mm::comm::testing::kSpoePacketLast);
  EXPECT_EQ(middle.status & 0xFF, mm::comm::testing::kSpoeReplyAck);
  ASSERT_EQ(middle.data.size(), 4 * mm::comm::testing::kSpoeEntrySize);

  // Subindex 0 of a record carries the subindex count in `value`, at offset 12.
  const std::size_t record = mm::comm::testing::kSpoeEntrySize;
  EXPECT_EQ(u16At(middle.data, record), 0x1018);
  EXPECT_EQ(middle.data[record + 2], 0);
  EXPECT_EQ(middle.data[record + 6], 0x09);
  EXPECT_EQ(middle.data[record + 12], 2);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(&middle.data[record + 16])), "Identity");
}

TEST(FakeSpoeServer, ParameterListCountKeepsOnlyTheLowByte) {
  FakeSpoeServer server;
  for (uint16_t i = 0; i < 200; ++i) {
    server.describeEntry(static_cast<uint16_t>(0x2000 + i), 0, kByteEntry);
  }
  for (uint16_t i = 0; i < 60; ++i) {
    server.describeEntry(static_cast<uint16_t>(0x3000 + i), 0, kByteEntry);
  }
  Client client(server.port());
  // 260 objects are capped at 200, which still fits one byte.
  const Frame first =
      client.request(SpoeMessage::kParamFullDesc, 1, {}, mm::comm::testing::kSpoePacketFirst);
  EXPECT_EQ(first.data[0], 200);
}

TEST(FakeSpoeServer, ALostParameterListPacketMovesTheListOnAnyway) {
  FakeSpoeServer server;
  for (uint16_t i = 0; i < 10; ++i) {
    server.describeEntry(static_cast<uint16_t>(0x2000 + i), 0, kByteEntry);
  }
  Client client(server.port());
  client.request(SpoeMessage::kParamFullDesc, 1, {}, mm::comm::testing::kSpoePacketFirst);
  server.injectFault(SpoeFault::kLoseParamListPacket);
  const Frame lost =
      client.request(SpoeMessage::kParamFullDesc, 2, {}, mm::comm::testing::kSpoePacketMiddle);
  EXPECT_EQ(lost.status & 0xFF, mm::comm::testing::kSpoeReplyBusy);
  EXPECT_TRUE(lost.data.empty());
  // The first seven objects are gone. The next packet holds the last three.
  const Frame rest =
      client.request(SpoeMessage::kParamFullDesc, 3, {}, mm::comm::testing::kSpoePacketMiddle);
  EXPECT_EQ(packetState(rest), mm::comm::testing::kSpoePacketLast);
  ASSERT_EQ(rest.data.size(), 3 * mm::comm::testing::kSpoeEntrySize);
  EXPECT_EQ(u16At(rest.data, 0), 0x2007);
}

TEST(FakeSpoeServer, AnEntryDescriptionComesAlone) {
  FakeSpoeServer server;
  describeSmallDictionary(server);
  Client client(server.port());
  const Frame frame = client.request(SpoeMessage::kParamSubDesc, 1, address(0x1018, 2));
  EXPECT_EQ(frame.status, 0);
  ASSERT_EQ(frame.data.size(), mm::comm::testing::kSpoeEntrySize);
  EXPECT_EQ(u16At(frame.data, 0), 0x1018);
  EXPECT_EQ(frame.data[2], 2);
}

TEST(FakeSpoeServer, ReadsAFileInPackets) {
  // `AppSockIf_StartReadingFile` only opens the file, so the first reply is empty.
  FakeSpoeServer server;
  server.setFile("config.csv", std::vector<uint8_t>(600, 'x'));
  Client client(server.port());
  const Frame first =
      client.request(SpoeMessage::kFileRead, 1, {'c', 'o', 'n', 'f', 'i', 'g', '.', 'c', 's', 'v'},
                     mm::comm::testing::kSpoePacketFirst);
  EXPECT_EQ(first.status & 0xFF, mm::comm::testing::kSpoeReplyAck);
  EXPECT_TRUE(first.data.empty());
  const Frame second =
      client.request(SpoeMessage::kFileRead, 2, {}, mm::comm::testing::kSpoePacketMiddle);
  EXPECT_EQ(second.data.size(), 512U);
  EXPECT_EQ(packetState(second), mm::comm::testing::kSpoePacketMiddle);
  const Frame third =
      client.request(SpoeMessage::kFileRead, 3, {}, mm::comm::testing::kSpoePacketMiddle);
  EXPECT_EQ(third.data.size(), 88U);
  EXPECT_EQ(packetState(third), mm::comm::testing::kSpoePacketLast);
}

TEST(FakeSpoeServer, TheComFirmwareInBootAnswersZeroForItsFirstPacket) {
  FakeSpoeServer server;
  server.setState(mm::comm::testing::kSpoeStateBoot);
  Client client(server.port());
  const std::string name = "com_firmware.bin";
  const Frame first = client.request(SpoeMessage::kFileWrite, 1, {name.begin(), name.end()},
                                     mm::comm::testing::kSpoePacketFirst);
  EXPECT_EQ(first.status & 0xFF, 0x00);
  const Frame last =
      client.request(SpoeMessage::kFileWrite, 2, {1, 2, 3}, mm::comm::testing::kSpoePacketLast);
  EXPECT_EQ(last.status & 0xFF, mm::comm::testing::kSpoeReplyAck);
  EXPECT_EQ(server.file(name), (std::vector<uint8_t>{1, 2, 3}));
}

TEST(FakeSpoeServer, TheFirmwareUpdateRestartsTheDriveInPreOp) {
  FakeSpoeServer server;
  server.setRestartTiming(milliseconds{100}, milliseconds{200});
  server.setState(mm::comm::testing::kSpoeStateBoot);
  Client client(server.port());
  EXPECT_EQ(client.request(SpoeMessage::kFirmwareUpdate, 1).status, 0);
  EXPECT_TRUE(client.closed());
  EXPECT_EQ(server.firmwareUpdates(), 1);
  std::this_thread::sleep_for(milliseconds{300});
  Client after(server.port());
  EXPECT_EQ(after.request(SpoeMessage::kStateRead, 1).data,
            (std::vector<uint8_t>{mm::comm::testing::kSpoeStatePreOp}));
}

TEST(FakeSpoeServer, SplitReplyArrivesWhole) {
  FakeSpoeServer server;
  server.setObject(0x6064, 0, {0x01, 0x02, 0x03, 0x04});
  server.injectFault(SpoeFault::kSplitReply);
  Client client(server.port());
  const Frame frame = client.request(SpoeMessage::kSdoRead, 1, address(0x6064, 0));
  EXPECT_EQ(frame.sequenceId, 1);
  EXPECT_EQ(frame.data, (std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04}));
}

TEST(FakeSpoeServer, HeldReplyArrivesWithTheNextOne) {
  FakeSpoeServer server;
  server.injectFault(SpoeFault::kHoldReply);
  Client client(server.port());
  client.send(SpoeMessage::kStateRead, 1);
  EXPECT_FALSE(client.receive(milliseconds{100}).has_value());
  client.send(SpoeMessage::kStateRead, 2);
  const auto late = client.receive();
  const auto current = client.receive();
  ASSERT_TRUE(late.has_value());
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(late->sequenceId, 1);
  EXPECT_EQ(current->sequenceId, 2);
}

TEST(FakeSpoeServer, WrongSequenceIdDoesNotMatchTheRequest) {
  FakeSpoeServer server;
  server.injectFault(SpoeFault::kWrongSequenceId);
  Client client(server.port());
  EXPECT_NE(client.request(SpoeMessage::kStateRead, 5).sequenceId, 5);
  EXPECT_EQ(client.request(SpoeMessage::kStateRead, 6).sequenceId, 6);
}

TEST(FakeSpoeServer, OversizeLengthClaimsMoreThanAFrame) {
  FakeSpoeServer server;
  server.injectFault(SpoeFault::kOversizeLength);
  Client client(server.port());
  EXPECT_GT(client.request(SpoeMessage::kStateRead, 1).length, kSpoeMaxDataSize);
}

TEST(FakeSpoeServer, NoReplySendsNothingAndTheNextRequestWorks) {
  FakeSpoeServer server;
  server.injectFault(SpoeFault::kNoReply);
  Client client(server.port());
  client.send(SpoeMessage::kStateRead, 1);
  EXPECT_FALSE(client.receive(milliseconds{100}).has_value());
  EXPECT_EQ(client.request(SpoeMessage::kStateRead, 2).sequenceId, 2);
}

TEST(FakeSpoeServer, DropConnectionClosesInsteadOfAReply) {
  FakeSpoeServer server;
  server.injectFault(SpoeFault::kDropConnection);
  Client client(server.port());
  client.send(SpoeMessage::kStateRead, 1);
  EXPECT_TRUE(client.closed());
}

TEST(FakeSpoeServer, ReplyDelayHoldsTheReplyBack) {
  FakeSpoeServer server;
  server.setReplyDelay(milliseconds{200});
  Client client(server.port());
  client.send(SpoeMessage::kStateRead, 1);
  EXPECT_FALSE(client.receive(milliseconds{50}).has_value());
  // The first receive gave up before any byte arrived. The reply still arrives, in one piece.
  EXPECT_TRUE(client.receive(milliseconds{1000}).has_value());
}

TEST(FakeSpoeServer, ASecondClientIsConnectedAndNeverAnswered) {
  // `Sock_SaveHandle` has one client slot. The second connection is accepted, its handle is
  // dropped, and the first client keeps the drive.
  FakeSpoeServer server;
  Client first(server.port());
  first.request(SpoeMessage::kStateRead, 1);
  Client second(server.port());
  second.send(SpoeMessage::kStateRead, 1);
  EXPECT_FALSE(second.receive(milliseconds{200}).has_value());
  EXPECT_EQ(first.request(SpoeMessage::kStateRead, 2).sequenceId, 2);
  EXPECT_EQ(server.ignoredConnectionCount(), 1);

  // The slot frees when the first client goes. A new client is served. The ignored one stays
  // silent, because the firmware no longer holds its handle.
  server.dropClient();
  Client third(server.port());
  EXPECT_EQ(third.request(SpoeMessage::kStateRead, 1).status, 0);
  second.send(SpoeMessage::kStateRead, 2);
  EXPECT_FALSE(second.receive(milliseconds{200}).has_value());
}

TEST(FakeSpoeServer, DropClientClosesTheConnectionAndAcceptsAReconnect) {
  FakeSpoeServer server;
  Client first(server.port());
  first.request(SpoeMessage::kStateRead, 1);
  server.dropClient();
  EXPECT_TRUE(first.closed());
  Client second(server.port());
  EXPECT_EQ(second.request(SpoeMessage::kStateRead, 1).status, 0);
  EXPECT_EQ(server.connectionCount(), 2);
}

}  // namespace
