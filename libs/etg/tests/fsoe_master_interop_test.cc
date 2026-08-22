#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "etg/fsoe_master.h"

namespace mm::etg {
namespace {

/// One bus cycle of a recorded exchange: what arrived, and what the master answered.
struct Exchange {
  std::vector<uint8_t> fromSlave;
  std::vector<uint8_t> expectedFromMaster;
};

/// A real exchange with the Synapticon drive firmware, recorded octet for octet.
///
/// The peer was the FSoE slave state machine in the Jasper firmware
/// (@c libs/etg5100_fsoe), the code that runs on the drive and that passes the FSoE conformance
/// test. The two state machines were linked into one process and wired back to back by
/// @c tools/fsoe_master_interop in that repository, which is also how this table is regenerated.
///
/// The recording is deterministic because every input is fixed: 8 SafeData octets out and 12 back,
/// slave address 3, connection ID 7, a 100 ms watchdog, no application parameters, master session
/// ID 0x2211, slave session ID 0x9ABC, SafeOutputs 0x01 in octet 0, and the SafeInputs below.
///
/// This is the test that says the master interoperates rather than merely being self-consistent. A
/// peer written from the same reading of the standard as the master would repeat any
/// misinterpretation; this one was written from the standard independently, and its behaviour was
/// measured on hardware.
const std::vector<Exchange>& recordedHandshake() {
  static const std::vector<Exchange> kExchanges = {
      // The master opens with a Reset PDU. The slave answers Reset, which is its acknowledgement.
      Exchange{
          .fromSlave = {},
          .expectedFromMaster = {0x2A, 0x00, 0x00, 0xC4, 0x2D, 0x00, 0x00, 0xB9, 0x14, 0x00, 0x00,
                                 0x9C, 0x02, 0x00, 0x00, 0x7F, 0x0F, 0x00, 0x00},
      },
      // Session: the master sends session ID 0x2211.
      Exchange{
          .fromSlave = {0x2A, 0x00, 0x00, 0xC4, 0x2D, 0x00, 0x00, 0xB9, 0x14,
                        0x00, 0x00, 0x9C, 0x02, 0x00, 0x00, 0x7F, 0x0F, 0x00,
                        0x00, 0xD6, 0x2E, 0x00, 0x00, 0x35, 0x23, 0x00, 0x00},
          .expectedFromMaster = {0x4E, 0x11, 0x22, 0xB7, 0x1F, 0x00, 0x00, 0x43, 0x4E, 0x00, 0x00,
                                 0x66, 0x58, 0x00, 0x00, 0x85, 0x55, 0x00, 0x00},
      },
      // Connection: connection ID 7 and slave address 3, and the connection ID appears in the
      // frame's own ConnId field from here on.
      Exchange{
          .fromSlave = {0x4E, 0xBD, 0x9A, 0x07, 0x26, 0x00, 0x00, 0x66, 0xC0,
                        0x00, 0x00, 0x43, 0xD6, 0x00, 0x00, 0xA0, 0xDB, 0x00,
                        0x00, 0x09, 0xFA, 0x00, 0x00, 0xEA, 0xF7, 0x00, 0x00},
          .expectedFromMaster = {0x64, 0x07, 0x00, 0x4A, 0x1A, 0x03, 0x00, 0x02, 0xB1, 0x00, 0x00,
                                 0x15, 0x31, 0x00, 0x00, 0xF6, 0x3C, 0x07, 0x00},
      },
      // Parameter: CommParaLen 2, watchdog 0x0064 (100 ms), AppParaLen 0.
      Exchange{
          .fromSlave = {0x64, 0x07, 0x00, 0xF5, 0x65, 0x03, 0x00, 0x1D, 0x08,
                        0x00, 0x00, 0x0A, 0x88, 0x00, 0x00, 0xE9, 0x85, 0x00,
                        0x00, 0x40, 0xA4, 0x00, 0x00, 0xA3, 0xA9, 0x07, 0x00},
          .expectedFromMaster = {0x52, 0x02, 0x00, 0x27, 0xF7, 0x64, 0x00, 0x32, 0x86, 0x00, 0x00,
                                 0xF2, 0xED, 0x00, 0x00, 0x11, 0xE0, 0x07, 0x00},
      },
      // The slave echoed the SafePara, so the connection is up and this frame carries the first
      // real SafeOutputs.
      Exchange{
          .fromSlave = {0x52, 0x02, 0x00, 0xCB, 0x3C, 0x64, 0x00, 0xFD, 0x4E,
                        0x00, 0x00, 0x3D, 0x25, 0x00, 0x00, 0xDE, 0x28, 0x00,
                        0x00, 0x77, 0x09, 0x00, 0x00, 0x94, 0x04, 0x07, 0x00},
          .expectedFromMaster = {0x36, 0x01, 0x00, 0x6F, 0x3E, 0x00, 0x00, 0x16, 0x22, 0x00, 0x00,
                                 0x33, 0x34, 0x00, 0x00, 0xD0, 0x39, 0x07, 0x00},
      },
      // From here the exchange is cyclic ProcessData. The SafeData never changes and every CRC
      // does, because the sequence numbers advance on both sides.
      Exchange{
          .fromSlave = {0x36, 0x01, 0x1F, 0x79, 0x53, 0x10, 0x20, 0xCB, 0x02,
                        0x30, 0x40, 0x8E, 0x1F, 0x50, 0x60, 0x57, 0xDE, 0x70,
                        0x80, 0x04, 0x25, 0x11, 0x22, 0x54, 0x43, 0x07, 0x00},
          .expectedFromMaster = {0x36, 0x01, 0x00, 0x42, 0xA2, 0x00, 0x00, 0x6F, 0x68, 0x00, 0x00,
                                 0x4A, 0x7E, 0x00, 0x00, 0xA9, 0x73, 0x07, 0x00},
      },
      Exchange{
          .fromSlave = {0x36, 0x01, 0x1F, 0x9E, 0x4F, 0x10, 0x20, 0xF2, 0x04,
                        0x30, 0x40, 0xB7, 0x19, 0x50, 0x60, 0x6E, 0xD8, 0x70,
                        0x80, 0x3D, 0x23, 0x11, 0x22, 0x6D, 0x45, 0x07, 0x00},
          .expectedFromMaster = {0x36, 0x01, 0x00, 0x1B, 0x1E, 0x00, 0x00, 0xC9, 0x28, 0x00, 0x00,
                                 0xEC, 0x3E, 0x00, 0x00, 0x0F, 0x33, 0x07, 0x00},
      },
      Exchange{
          .fromSlave = {0x36, 0x01, 0x1F, 0x1F, 0xD3, 0x10, 0x20, 0x47, 0xDE,
                        0x30, 0x40, 0x02, 0xC3, 0x50, 0x60, 0xDB, 0x02, 0x70,
                        0x80, 0x88, 0xF9, 0x11, 0x22, 0xD8, 0x9F, 0x07, 0x00},
          .expectedFromMaster = {0x36, 0x01, 0x00, 0x07, 0x1C, 0x00, 0x00, 0x5B, 0x88, 0x00, 0x00,
                                 0x7E, 0x9E, 0x00, 0x00, 0x9D, 0x93, 0x07, 0x00},
      },
  };
  return kExchanges;
}

/// The SafeInputs the firmware slave was told to report, and the SafeOutputs the master sent.
constexpr std::array<uint8_t, 12> kRecordedInputs{0x01, 0x1F, 0x10, 0x20, 0x30, 0x40,
                                                  0x50, 0x60, 0x70, 0x80, 0x11, 0x22};
constexpr std::array<uint8_t, 8> kRecordedOutputs{0x01, 0, 0, 0, 0, 0, 0, 0};

std::expected<FsoeMaster, std::string> createRecordedMaster() {
  return FsoeMaster::create({.safeOutputsLen = 8,
                             .safeInputsLen = 12,
                             .slaveAddress = 3,
                             .connectionId = 7,
                             .watchdogMs = 100,
                             .applicationParameters = {},
                             .initialSessionId = 0x2211});
}

TEST(FsoeMasterInteropTest, ReproducesEveryOctetOfARecordedDriveExchange) {
  auto created = createRecordedMaster();
  ASSERT_TRUE(created.has_value()) << (created ? "" : created.error());
  FsoeMaster master = std::move(*created);
  ASSERT_TRUE(master.setDataCommand(FsoeCommand::ProcessData));
  ASSERT_TRUE(master.setSafeOutputs(kRecordedOutputs));

  int cycle = 0;
  for (const Exchange& exchange : recordedHandshake()) {
    ++cycle;
    const auto result = master.cycle(exchange.fromSlave, 1000);
    ASSERT_TRUE(result.has_value()) << "cycle " << cycle << ": " << result.error();
    EXPECT_EQ(result->fault, FsoeError::None) << "cycle " << cycle;

    const std::vector<uint8_t> actual(master.txPdu().begin(), master.txPdu().end());
    EXPECT_EQ(actual, exchange.expectedFromMaster) << "cycle " << cycle;
  }

  EXPECT_EQ(master.state(), FsoeState::Data);
  EXPECT_TRUE(master.inputsValid());
  EXPECT_TRUE(std::ranges::equal(master.safeInputs(), kRecordedInputs));
  EXPECT_EQ(master.sessionId(), 0x2211);
}

TEST(FsoeMasterInteropTest, TheRecordedFramesCarryTheLengthsTheDriveUses) {
  // A drive with the safe-sensor option takes 8 SafeData octets and returns 12, so the frames are
  // 19 and 27 octets. The trace is the record that the asymmetry is real and not a reading of the
  // specification: these are the octet counts the firmware accepted and produced.
  for (const Exchange& exchange : recordedHandshake()) {
    if (!exchange.fromSlave.empty()) {
      EXPECT_EQ(exchange.fromSlave.size(), 27u);
    }
    EXPECT_EQ(exchange.expectedFromMaster.size(), 19u);
  }
}

TEST(FsoeMasterInteropTest, ASingleAlteredOctetInTheTraceIsRejected) {
  // The replay above would still pass if the master ignored the frames and produced its own
  // sequence by luck. This flips one octet of one recorded slave frame, in a field the CRC covers,
  // and requires the master to fault instead of carrying on.
  auto created = createRecordedMaster();
  ASSERT_TRUE(created.has_value());
  FsoeMaster master = std::move(*created);
  ASSERT_TRUE(master.setDataCommand(FsoeCommand::ProcessData));
  ASSERT_TRUE(master.setSafeOutputs(kRecordedOutputs));

  const std::vector<Exchange>& exchanges = recordedHandshake();
  FsoeError fault = FsoeError::None;
  for (size_t i = 0; i < exchanges.size() && fault == FsoeError::None; ++i) {
    std::vector<uint8_t> frame = exchanges[i].fromSlave;
    if (i == 5) {
      frame[2] ^= 0x01;  // A SafeData octet of a ProcessData frame in the Data state.
    }
    const auto result = master.cycle(frame, 1000);
    ASSERT_TRUE(result.has_value()) << result.error();
    fault = result->fault;
  }

  EXPECT_EQ(fault, FsoeError::InvalidCrc);
  EXPECT_EQ(master.state(), FsoeState::Reset);
  EXPECT_FALSE(master.inputsValid());
}

}  // namespace
}  // namespace mm::etg
