#include "etg/fsoe_master.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "etg/tests/fake_fsoe_slave.h"

namespace mm::etg {
namespace {

constexpr uint16_t kSlaveAddress = 3;
constexpr uint16_t kConnectionId = 7;
constexpr uint16_t kWatchdogMs = 100;
constexpr uint32_t kCycleUs = 1000;

/// A master and a slave wired together, one cycle of transport lag between them.
///
/// A real fieldbus presents the frame the peer sent in an earlier cycle, never the answer to the
/// frame going out now. The lag is modelled here, because a handshake that only works without it
/// would fail on hardware.
class Link {
 public:
  Link(uint16_t outLen, uint16_t inLen, std::vector<uint8_t> applicationParameters = {})
      : master_(*FsoeMaster::create({.safeOutputsLen = outLen,
                                     .safeInputsLen = inLen,
                                     .slaveAddress = kSlaveAddress,
                                     .connectionId = kConnectionId,
                                     .watchdogMs = kWatchdogMs,
                                     .applicationParameters = applicationParameters,
                                     .initialSessionId = 0x2211})),
        slave_(inLen, outLen, kSlaveAddress,
               static_cast<uint16_t>(6 + applicationParameters.size())) {}

  /// Runs one bus cycle. The slave sees the master's new frame; the master sees the answer to the
  /// previous one.
  FsoeCycleResult cycle(uint32_t dtUs = kCycleUs) {
    const auto result = master_.cycle(fromSlave_, dtUs);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error());
    if (deliverToSlave_) {
      const auto answer = slave_.process(master_.txPdu());
      fromSlave_.assign(answer.begin(), answer.end());
    }
    return result.value_or(FsoeCycleResult{});
  }

  /// Runs cycles until the connection is up, or gives up.
  void runHandshake(int maxCycles = 12) {
    for (int i = 0; i < maxCycles && master_.state() != FsoeState::Data; ++i) {
      cycle();
    }
  }

  /// Stops feeding the master's frames to the slave, so the answers stop.
  void unplugSlave() { deliverToSlave_ = false; }

  /// Replaces the frame the master will read next cycle.
  void injectFromSlave(std::vector<uint8_t> frame) { fromSlave_ = std::move(frame); }

  FsoeMaster& master() { return master_; }
  FakeFsoeSlave& slave() { return slave_; }
  const std::vector<uint8_t>& fromSlave() const { return fromSlave_; }

 private:
  FsoeMaster master_;
  FakeFsoeSlave slave_;
  std::vector<uint8_t> fromSlave_;
  bool deliverToSlave_ = true;
};

TEST(FsoeMasterTest, AFreshMasterAlreadyHoldsTheResetPduThatStartsTheHandshake) {
  // ETG.5100 requires the Reset Connection event on power-on. Building the frame in the
  // constructor removes the start step a caller could forget, which would look like a slave that
  // never answers.
  const auto master = FsoeMaster::create({.safeOutputsLen = 8,
                                          .safeInputsLen = 12,
                                          .slaveAddress = kSlaveAddress,
                                          .connectionId = kConnectionId});
  ASSERT_TRUE(master.has_value()) << (master ? "" : master.error());
  EXPECT_EQ(master->state(), FsoeState::Reset);
  EXPECT_EQ(master->txPdu().size(), 19u);
  EXPECT_EQ(master->rxPduSize(), 27u);
  EXPECT_EQ(fsoeFrameCommand(master->txPdu()), static_cast<uint8_t>(FsoeCommand::Reset));
  EXPECT_FALSE(master->inputsValid());
}

TEST(FsoeMasterTest, CompletesTheHandshakeAndCarriesProcessDataBothWays) {
  Link link(8, 12);
  constexpr std::array<uint8_t, 12> inputs{0x01, 0x1F, 0xAA, 0xBB, 0xCC, 0xDD,
                                           0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  constexpr std::array<uint8_t, 8> outputs{0x01, 0, 0, 0, 0, 0, 0, 0};
  link.slave().setSafeInputs(inputs);
  ASSERT_TRUE(link.master().setSafeOutputs(outputs));
  ASSERT_TRUE(link.master().setDataCommand(FsoeCommand::ProcessData));

  link.runHandshake();

  EXPECT_EQ(link.master().state(), FsoeState::Data);
  EXPECT_EQ(link.slave().state(), FsoeState::Data);

  // Two more cycles: one to carry the outputs, one to bring the inputs back.
  link.cycle();
  link.cycle();

  EXPECT_TRUE(link.master().inputsValid());
  EXPECT_TRUE(std::ranges::equal(link.master().safeInputs(), inputs));
  EXPECT_TRUE(link.slave().outputsValid());
  EXPECT_TRUE(std::ranges::equal(link.slave().safeOutputs(), outputs));
}

TEST(FsoeMasterTest, WalksTheStatesInTheOrderTheStandardDefines) {
  Link link(8, 8);
  std::vector<FsoeState> seen{link.master().state()};
  for (int i = 0; i < 10; ++i) {
    link.cycle();
    if (link.master().state() != seen.back()) {
      seen.push_back(link.master().state());
    }
  }
  EXPECT_EQ(seen,
            (std::vector<FsoeState>{FsoeState::Reset, FsoeState::Session, FsoeState::Connection,
                                    FsoeState::Parameter, FsoeState::Data}));
}

TEST(FsoeMasterTest, SendsTheSafeParaTheSlaveExpects) {
  Link link(8, 8);
  link.runHandshake();
  ASSERT_EQ(link.master().state(), FsoeState::Data);

  // CommParaLen 2, the watchdog, AppParaLen 0. The slave refuses the connection on any other
  // shape, so reaching Data is already most of the proof; this pins the octets.
  EXPECT_EQ(link.master().safePara().size(), 6u);
  EXPECT_TRUE(std::ranges::equal(link.master().safePara(),
                                 std::vector<uint8_t>{0x02, 0x00, kWatchdogMs, 0x00, 0x00, 0x00}));
  EXPECT_TRUE(std::ranges::equal(link.slave().safePara(), link.master().safePara()));
  EXPECT_EQ(link.slave().connectionId(), kConnectionId);
}

TEST(FsoeMasterTest, SplitsASafeParaThatDoesNotFitOneFrame) {
  // Any device with application parameters needs several Parameter cycles. With 4 SafeData octets
  // and 8 application parameters the SafePara takes 14 octets, so the transfer runs 4 frames.
  const std::vector<uint8_t> appParameters{1, 2, 3, 4, 5, 6, 7, 8};
  Link link(4, 4, appParameters);
  link.runHandshake(20);

  EXPECT_EQ(link.master().state(), FsoeState::Data);
  EXPECT_EQ(link.master().safePara().size(), 14u);
  EXPECT_TRUE(std::ranges::equal(link.slave().safePara(), link.master().safePara()));
}

TEST(FsoeMasterTest, RepeatedInputOctetsAreNotANewFrame) {
  // A fieldbus re-presents the same input image until the peer writes new data. Treating a repeat
  // as an event would answer one slave frame twice, and the second answer carries a sequence
  // number the slave is not expecting.
  Link link(8, 8);
  link.runHandshake();
  ASSERT_EQ(link.master().state(), FsoeState::Data);

  // With the slave unplugged the pending answer stays in the input image, exactly as a fieldbus
  // re-presents it.
  link.unplugSlave();
  link.cycle();
  const std::vector<uint8_t> before(link.master().txPdu().begin(), link.master().txPdu().end());

  const FsoeCycleResult result = link.cycle();
  EXPECT_FALSE(result.txUpdated);
  EXPECT_FALSE(result.rxAccepted);
  EXPECT_TRUE(std::ranges::equal(link.master().txPdu(), before));
}

TEST(FsoeMasterTest, TheWatchdogTakesTheConnectionDownWhenTheAnswersStop) {
  Link link(8, 8);
  link.runHandshake();
  ASSERT_EQ(link.master().state(), FsoeState::Data);
  link.unplugSlave();

  FsoeError fault = FsoeError::None;
  for (int i = 0; i < 200 && fault == FsoeError::None; ++i) {
    fault = link.cycle().fault;
  }

  EXPECT_EQ(fault, FsoeError::WatchdogExpired);
  EXPECT_EQ(link.master().state(), FsoeState::Reset);
  EXPECT_EQ(link.master().faultReason(), FsoeError::WatchdogExpired);
  EXPECT_FALSE(link.master().inputsValid());
  // The safe inputs read fail-safe, whether or not the caller looked at the flag.
  EXPECT_TRUE(std::ranges::all_of(link.master().safeInputs(), [](uint8_t v) { return v == 0; }));
}

TEST(FsoeMasterTest, TheWatchdogFiresOnlyAfterTheConfiguredTime) {
  Link link(8, 8);
  link.runHandshake();
  link.unplugSlave();
  link.cycle();  // Consume the answer already in the input image, which arms the watchdog.

  // 99 ms of silence is not yet a fault. The next millisecond is.
  for (int i = 0; i < 99; ++i) {
    EXPECT_EQ(link.cycle().fault, FsoeError::None) << "cycle " << i;
  }
  EXPECT_EQ(link.cycle().fault, FsoeError::WatchdogExpired);
}

TEST(FsoeMasterTest, ACorruptedAnswerInTheDataStateFaultsWithInvalidCrc) {
  Link link(8, 8);
  link.runHandshake();
  ASSERT_EQ(link.master().state(), FsoeState::Data);

  link.slave().corruptNextCrc();
  link.cycle();  // The slave builds the corrupted answer.
  const FsoeCycleResult result = link.cycle();

  EXPECT_EQ(result.fault, FsoeError::InvalidCrc);
  EXPECT_EQ(link.master().state(), FsoeState::Reset);
  EXPECT_FALSE(link.master().inputsValid());
  EXPECT_EQ(fsoeFrameCommand(link.master().txPdu()), static_cast<uint8_t>(FsoeCommand::Reset));
}

TEST(FsoeMasterTest, AnAnswerCarryingAnotherConnectionIdIsRefused) {
  // Two connections on one bus must not be able to answer for each other. The connection ID is
  // checked before the CRC, so the master reports the useful reason rather than a CRC mismatch.
  Link link(8, 8);
  link.runHandshake();
  ASSERT_EQ(link.master().state(), FsoeState::Data);
  link.cycle();

  std::vector<uint8_t> frame = link.fromSlave();
  const FsoeFrameLayout layout(8);
  fsoeFrameSetConnId(frame, layout, kConnectionId + 1);
  link.injectFromSlave(frame);

  EXPECT_EQ(link.cycle().fault, FsoeError::InvalidConnId);
  EXPECT_EQ(link.master().state(), FsoeState::Reset);
}

TEST(FsoeMasterTest, AnUnexpectedCommandAndAnUnknownOneAreReportedApart) {
  Link link(8, 8);
  link.runHandshake();
  ASSERT_EQ(link.master().state(), FsoeState::Data);
  link.cycle();

  std::vector<uint8_t> frame = link.fromSlave();
  fsoeFrameSetCommand(frame, static_cast<uint8_t>(FsoeCommand::Parameter));
  link.injectFromSlave(frame);
  EXPECT_EQ(link.cycle().fault, FsoeError::UnexpectedCommand);

  Link other(8, 8);
  other.runHandshake();
  other.cycle();
  std::vector<uint8_t> unknown = other.fromSlave();
  fsoeFrameSetCommand(unknown, 0x99);
  other.injectFromSlave(unknown);
  EXPECT_EQ(other.cycle().fault, FsoeError::UnknownCommand);
}

TEST(FsoeMasterTest, FailSafeDataFromTheSlaveClearsTheInputsAndHoldsTheConnection) {
  Link link(8, 8);
  constexpr std::array<uint8_t, 8> inputs{1, 2, 3, 4, 5, 6, 7, 8};
  link.slave().setSafeInputs(inputs);
  link.slave().setDataCommand(FsoeCommand::ProcessData);
  link.runHandshake();
  link.cycle();
  link.cycle();
  ASSERT_TRUE(link.master().inputsValid());

  link.slave().setDataCommand(FsoeCommand::FailSafeData);
  link.cycle();
  link.cycle();

  // The connection stays up. Only the data is fail-safe, which is what lets a slave report a
  // safety function as active without dropping the link.
  EXPECT_EQ(link.master().state(), FsoeState::Data);
  EXPECT_FALSE(link.master().inputsValid());
  EXPECT_TRUE(std::ranges::all_of(link.master().safeInputs(), [](uint8_t v) { return v == 0; }));
}

TEST(FsoeMasterTest, SendingFailSafeDataDropsTheSlaveOutputsWithoutANewHandshake) {
  Link link(8, 8);
  constexpr std::array<uint8_t, 8> outputs{0x01, 0, 0, 0, 0, 0, 0, 0};
  ASSERT_TRUE(link.master().setSafeOutputs(outputs));
  ASSERT_TRUE(link.master().setDataCommand(FsoeCommand::ProcessData));
  link.runHandshake();
  link.cycle();
  ASSERT_TRUE(link.slave().outputsValid());

  ASSERT_TRUE(link.master().setDataCommand(FsoeCommand::FailSafeData));
  link.cycle();

  EXPECT_EQ(link.slave().state(), FsoeState::Data);
  EXPECT_FALSE(link.slave().outputsValid());
  EXPECT_TRUE(std::ranges::all_of(link.slave().safeOutputs(), [](uint8_t v) { return v == 0; }));
}

TEST(FsoeMasterTest, AResetFromTheSlaveRestartsTheHandshakeAndKeepsTheReason) {
  Link link(8, 8);
  link.runHandshake();
  ASSERT_EQ(link.master().state(), FsoeState::Data);

  // Corrupt the frame on its way to the slave. The slave faults, and its Reset PDU names why.
  std::vector<uint8_t> corrupted(link.master().txPdu().begin(), link.master().txPdu().end());
  corrupted[2] ^= 0x01;
  const auto answer = link.slave().process(corrupted);
  ASSERT_EQ(fsoeFrameCommand(answer), static_cast<uint8_t>(FsoeCommand::Reset));
  link.injectFromSlave(std::vector<uint8_t>(answer.begin(), answer.end()));

  link.cycle();

  EXPECT_EQ(link.master().state(), FsoeState::Session);
  EXPECT_EQ(link.master().peerFaultCode(), static_cast<uint8_t>(FsoeError::InvalidCrc));
  EXPECT_FALSE(link.master().inputsValid());

  // The connection comes back on its own.
  link.runHandshake();
  EXPECT_EQ(link.master().state(), FsoeState::Data);
}

TEST(FsoeMasterTest, EachSessionUsesANewSessionId) {
  Link link(8, 8);
  link.runHandshake();
  const uint16_t first = link.master().sessionId();
  EXPECT_EQ(first, 0x2211);

  link.master().resetConnection();
  link.runHandshake();
  EXPECT_NE(link.master().sessionId(), first);
}

TEST(FsoeMasterTest, ALocalResetDropsTheConnectionAtOnce) {
  Link link(8, 8);
  link.slave().setDataCommand(FsoeCommand::ProcessData);
  link.runHandshake();
  link.cycle();
  ASSERT_TRUE(link.master().inputsValid());

  link.master().resetConnection();

  EXPECT_EQ(link.master().state(), FsoeState::Reset);
  EXPECT_FALSE(link.master().inputsValid());
  EXPECT_EQ(link.master().faultReason(), FsoeError::None);
  EXPECT_EQ(fsoeFrameCommand(link.master().txPdu()), static_cast<uint8_t>(FsoeCommand::Reset));

  link.runHandshake();
  EXPECT_EQ(link.master().state(), FsoeState::Data);
}

TEST(FsoeMasterTest, RetriesOnItsOwnWhenTheSlaveNeverAnswers) {
  // A master that waits for a peer that never speaks is a master a user cannot diagnose. The
  // watchdog in the Reset state offers a Session anyway, so the connection keeps trying.
  Link link(8, 8);
  link.unplugSlave();
  for (int i = 0; i < 101; ++i) {
    link.cycle();
  }
  EXPECT_EQ(link.master().state(), FsoeState::Session);
}

TEST(FsoeMasterTest, RefusesAConfigurationTheProtocolCannotCarry) {
  const FsoeMasterConfig base{
      .safeOutputsLen = 8, .safeInputsLen = 8, .slaveAddress = 1, .connectionId = 1};

  auto oddLength = base;
  oddLength.safeOutputsLen = 3;
  EXPECT_FALSE(FsoeMaster::create(oddLength).has_value());

  auto noData = base;
  noData.safeInputsLen = 0;
  EXPECT_FALSE(FsoeMaster::create(noData).has_value());

  auto noConnection = base;
  noConnection.connectionId = 0;
  EXPECT_FALSE(FsoeMaster::create(noConnection).has_value());

  auto noWatchdog = base;
  noWatchdog.watchdogMs = 0;
  EXPECT_FALSE(FsoeMaster::create(noWatchdog).has_value());

  EXPECT_TRUE(FsoeMaster::create(base).has_value());
}

TEST(FsoeMasterTest, RefusesSafeOutputsOfTheWrongLength) {
  Link link(8, 8);
  constexpr std::array<uint8_t, 4> tooShort{1, 2, 3, 4};
  EXPECT_FALSE(link.master().setSafeOutputs(tooShort));
  EXPECT_FALSE(link.master().setDataCommand(FsoeCommand::Session));
}

TEST(FsoeMasterTest, ReportsAWrongSizedInputAsATransportDefectNotAsAFault) {
  // A short span means the transport is slicing the wrong part of the input image. Failing the
  // connection would send the integrator looking at the safety configuration instead.
  Link link(8, 12);
  const std::vector<uint8_t> tooShort(19, 0);
  const auto result = link.master().cycle(tooShort, kCycleUs);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("27"), std::string::npos) << result.error();
  EXPECT_EQ(link.master().state(), FsoeState::Reset);
}

TEST(FsoeMasterTest, AnEmptyInputSpanIsSilenceNotAnError) {
  Link link(8, 8);
  const auto result = link.master().cycle({}, kCycleUs);
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->rxAccepted);
  EXPECT_FALSE(result->txUpdated);
}

TEST(FsoeMasterTest, CarriesAsymmetricSafeDataLengths) {
  // A drive that takes 8 octets and returns 12 is the normal case, not an exotic one. The two
  // directions are sized independently, and a master that reuses one length for both builds a
  // frame the slave drops without a word.
  Link link(8, 12);
  link.runHandshake();
  link.cycle();
  link.cycle();

  EXPECT_EQ(link.master().state(), FsoeState::Data);
  EXPECT_EQ(link.master().txPdu().size(), 19u);
  EXPECT_EQ(link.master().rxPduSize(), 27u);
  EXPECT_EQ(link.master().safeInputs().size(), 12u);
}

}  // namespace
}  // namespace mm::etg
