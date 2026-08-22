#include "etg/fsoe_master.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

#include "etg/fsoe_crc.h"

namespace mm::etg {
namespace {

/// Octets of SafePara the master always sends before the application parameters: CommParaLen, the
/// watchdog, and AppParaLen, two octets each.
constexpr uint16_t kCommParaOctets = 6;

/// Value of CommParaLen. The communication half of the SafePara is the watchdog and nothing else,
/// so its length is fixed at two octets. A slave that reads any other value refuses the connection.
constexpr uint16_t kCommParaLen = 2;

/// SafeData octets of a Session PDU: the 16-bit session ID.
constexpr uint16_t kSessionIdOctets = 2;

/// SafeData octets of a Connection PDU: the connection ID and the FSoE Slave Address.
constexpr uint16_t kConnDataOctets = 4;

/// Advances a sequence number, wrapping to 1 rather than to 0. Zero is skipped because it marks
/// the state before the first frame of a session.
constexpr uint16_t nextSeq(uint16_t seq) {
  return seq == UINT16_MAX ? 1 : static_cast<uint16_t>(seq + 1);
}

constexpr void writeLe16(std::span<uint8_t> out, uint16_t v) {
  out[0] = static_cast<uint8_t>(v & 0xFF);
  out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

}  // namespace

FsoeMaster::FsoeMaster(FsoeMasterConfig config)
    : config_(std::move(config)),
      txLayout_(config_.safeOutputsLen),
      rxLayout_(config_.safeInputsLen),
      chunkLen_(std::min(config_.safeOutputsLen, config_.safeInputsLen)),
      tx_(txLayout_.size(), 0),
      txSafeData_(config_.safeOutputsLen, 0),
      rxSafeData_(config_.safeInputsLen, 0),
      lastRx_(rxLayout_.size(), 0),
      safeOutputs_(config_.safeOutputsLen, 0),
      safeInputs_(config_.safeInputsLen, 0),
      safePara_(kCommParaOctets + config_.applicationParameters.size(), 0),
      connData_(kConnDataOctets, 0),
      nextSessionId_(config_.initialSessionId) {
  writeLe16(std::span(safePara_).subspan(0, 2), kCommParaLen);
  writeLe16(std::span(safePara_).subspan(2, 2), config_.watchdogMs);
  writeLe16(std::span(safePara_).subspan(4, 2),
            static_cast<uint16_t>(config_.applicationParameters.size()));
  std::ranges::copy(config_.applicationParameters, safePara_.begin() + kCommParaOctets);

  writeLe16(std::span(connData_).subspan(0, 2), config_.connectionId);
  writeLe16(std::span(connData_).subspan(2, 2), config_.slaveAddress);

  // ETG.5100 requires the Reset Connection event on power-on to start communication, so a fresh
  // master already holds a Reset PDU. There is no separate start step to forget.
  resetConnection();
}

std::expected<FsoeMaster, std::string> FsoeMaster::create(FsoeMasterConfig config) {
  const FsoeFrameLayout txLayout(config.safeOutputsLen);
  const FsoeFrameLayout rxLayout(config.safeInputsLen);
  if (!txLayout.valid()) {
    return std::unexpected(std::format(
        "safeOutputsLen {} is not a legal SafeData length: use 1, or any positive even number",
        config.safeOutputsLen));
  }
  if (!rxLayout.valid()) {
    return std::unexpected(std::format(
        "safeInputsLen {} is not a legal SafeData length: use 1, or any positive even number",
        config.safeInputsLen));
  }
  if (config.connectionId == 0) {
    // A slave reads connection ID 0 as "no connection" and answers INVALID_CONNID.
    return std::unexpected("connectionId 0 is reserved; use a non-zero ID, unique on the bus");
  }
  if (config.watchdogMs == 0) {
    return std::unexpected("watchdogMs 0 would expire before the first response");
  }
  if (config.applicationParameters.size() > UINT16_MAX - kCommParaOctets) {
    return std::unexpected("applicationParameters is too long for the 16-bit AppParaLen field");
  }
  return FsoeMaster(std::move(config));
}

// ---- protocol primitives (ETG.5100 Tables 31 and 33) -------------------------------------------

uint16_t FsoeMaster::sendFrame(FsoeCommand command, std::span<const uint8_t> data,
                               uint16_t inheritCrc, uint16_t connId, bool calculateNewCrc) {
  fsoeFrameSetCommand(tx_, static_cast<uint8_t>(command));
  fsoeFrameSetConnId(tx_, txLayout_, connId);
  fsoeFrameWriteSafeData(tx_, txLayout_, data);

  // Keep what we sent. The slave echoes the first chunk of it during the handshake, and the
  // Connection and Parameter states compare the echo octet for octet.
  std::ranges::fill(txSafeData_, 0);
  std::ranges::copy(data.first(std::min(data.size(), txSafeData_.size())), txSafeData_.begin());

  std::array<uint8_t, 2> block0{};
  const uint8_t len0 = fsoeFrameReadBlock(tx_, txLayout_, 0, block0);
  const auto commandOctet = static_cast<uint8_t>(command);

  // ETG.5100 ch. 8.1.3.4: two successive frames must never carry the same CRC_0, or a receiver
  // cannot tell a repeat from a fresh frame. Advance the sequence number until they differ.
  uint16_t seq = masterSeq_;
  uint16_t used = seq;
  uint16_t crc0 = 0;
  do {
    used = seq;
    crc0 = fsoeCrc(inheritCrc, connId, used, commandOctet, 0, block0, len0);
    seq = nextSeq(seq);
  } while (calculateNewCrc && crc0 == oldMasterCrc_);
  fsoeFrameWriteCrc(tx_, txLayout_, 0, crc0);

  for (uint16_t k = 1; k < txLayout_.blockCount(); ++k) {
    std::array<uint8_t, 2> block{};
    const uint8_t len = fsoeFrameReadBlock(tx_, txLayout_, k, block);
    fsoeFrameWriteCrc(tx_, txLayout_, k,
                      fsoeCrc(inheritCrc, connId, used, commandOctet, k, block, len));
  }

  masterSeq_ = seq;
  oldMasterCrc_ = crc0;
  return crc0;
}

bool FsoeMaster::checkRecv(std::span<const uint8_t> rx, uint16_t inheritCrc, bool calculateNewCrc) {
  const uint8_t command = fsoeFrameCommand(rx);
  const uint16_t connId = fsoeFrameConnId(rx, rxLayout_);

  std::array<uint8_t, 2> block0{};
  const uint8_t len0 = fsoeFrameReadBlock(rx, rxLayout_, 0, block0);

  uint16_t seq = slaveSeq_;
  uint16_t used = seq;
  uint16_t crc0 = 0;
  do {
    used = seq;
    crc0 = fsoeCrc(inheritCrc, connId, used, command, 0, block0, len0);
    seq = nextSeq(seq);
  } while (calculateNewCrc && crc0 == oldSlaveCrc_);

  bool ok = crc0 == fsoeFrameReadCrc(rx, rxLayout_, 0);
  for (uint16_t k = 1; k < rxLayout_.blockCount(); ++k) {
    std::array<uint8_t, 2> block{};
    const uint8_t len = fsoeFrameReadBlock(rx, rxLayout_, k, block);
    if (fsoeCrc(inheritCrc, connId, used, command, k, block, len) !=
        fsoeFrameReadCrc(rx, rxLayout_, k)) {
      ok = false;
    }
  }

  // A rejected frame leaves the expected sequence number and the collision base untouched, so a
  // frame lost on the wire does not desynchronise the connection on its own.
  if (ok) {
    slaveSeq_ = seq;
    oldSlaveCrc_ = crc0;
  }
  return ok;
}

bool FsoeMaster::isSafeDataCorrect() const {
  // The slave echoes the chunk it received, and nothing more: min(in, out) octets, zero-filled to
  // its own frame length. So the check compares that one chunk, not the whole transfer.
  return std::equal(rxSafeData_.begin(), rxSafeData_.begin() + chunkLen_, txSafeData_.begin());
}

void FsoeMaster::updateBytesToBeSent(uint16_t remaining) {
  bytesToBeSent_ = remaining > chunkLen_ ? static_cast<uint16_t>(remaining - chunkLen_) : 0;
}

void FsoeMaster::startWatchdog() {
  watchdogElapsedUs_ = 0;
  watchdogRunning_ = true;
}

// ---- composite transitions ---------------------------------------------------------------------

void FsoeMaster::resetVars() {
  lastCrc_ = 0;
  oldMasterCrc_ = 0;
  oldSlaveCrc_ = 0;
  masterSeq_ = 1;
  slaveSeq_ = 1;
  bytesToBeSent_ = 0;
  secondSessionFrameSent_ = false;
  dataCommand_ = FsoeCommand::FailSafeData;
}

void FsoeMaster::goSafe() {
  std::ranges::fill(safeInputs_, 0);
  inputsValid_ = false;
}

FsoeError FsoeMaster::emitReset(FsoeError reason) {
  resetVars();
  goSafe();
  faultReason_ = reason;

  // SafeData[0] carries the reason, for the peer's diagnostics. Every other octet is zero.
  std::array<uint8_t, 2> data{static_cast<uint8_t>(reason), 0};
  (void)sendFrame(FsoeCommand::Reset, data, 0, 0, false);

  // A back-to-Reset transition ends with the sequence number and LastCrc back at their defaults,
  // so the next session starts from the same point on both sides.
  masterSeq_ = 1;
  lastCrc_ = 0;
  startWatchdog();
  state_ = FsoeState::Reset;
  return reason;
}

void FsoeMaster::startSession(bool resetFirst) {
  if (resetFirst) {
    resetVars();
    goSafe();
  }
  sessionId_ = nextSessionId_;
  nextSessionId_++;
  writeLe16(sessionIdBytes_, sessionId_);
  lastCrc_ = sendFrame(FsoeCommand::Session, sessionIdBytes_, lastCrc_, 0, false);
  updateBytesToBeSent(kSessionIdOctets);
  secondSessionFrameSent_ = false;
  startWatchdog();
  state_ = FsoeState::Session;
}

void FsoeMaster::sendChunk(FsoeCommand command, std::span<const uint8_t> source, uint16_t total,
                           uint16_t offset, uint16_t inheritCrc, uint16_t connId) {
  const auto remaining = static_cast<uint16_t>(total - offset);
  const uint16_t len = std::min(chunkLen_, remaining);
  lastCrc_ = sendFrame(command, source.subspan(offset, len), inheritCrc, connId, true);
  updateBytesToBeSent(remaining);
}

// ---- public API ---------------------------------------------------------------------------------

void FsoeMaster::resetConnection() {
  (void)emitReset(FsoeError::None);
  peerFaultCode_ = 0;
  // Forget the last received PDU: after a restart the slave may still be repeating a frame we have
  // already seen, and that frame is new information again.
  lastRxValid_ = false;
}

bool FsoeMaster::setDataCommand(FsoeCommand command) {
  if (command != FsoeCommand::ProcessData && command != FsoeCommand::FailSafeData) {
    return false;
  }
  dataCommand_ = command;
  return true;
}

bool FsoeMaster::setSafeOutputs(std::span<const uint8_t> data) {
  if (data.size() != safeOutputs_.size()) {
    return false;
  }
  std::ranges::copy(data, safeOutputs_.begin());
  return true;
}

std::expected<FsoeCycleResult, std::string> FsoeMaster::cycle(std::span<const uint8_t> rx,
                                                              uint32_t dtUs) {
  if (!rx.empty() && rx.size() != rxLayout_.size()) {
    return std::unexpected(
        std::format("received {} octets, expected {}: the transport is slicing "
                    "the wrong span out of the input image",
                    rx.size(), rxLayout_.size()));
  }

  FsoeCycleResult result{.state = state_, .txUpdated = false, .rxAccepted = false};

  // The watchdog runs first. A frame that arrives after the deadline is a late frame, and the
  // standard treats the deadline, not the arrival, as the event.
  if (watchdogRunning_) {
    watchdogElapsedUs_ += dtUs;
    if (watchdogElapsedUs_ >= static_cast<uint32_t>(config_.watchdogMs) * 1000) {
      if (state_ == FsoeState::Reset) {
        // RESET_WD. The slave never answered our Reset. Offer a Session anyway, which is what
        // makes the master retry on its own instead of waiting for a peer that may never speak.
        startSession(false);
      } else {
        result.fault = emitReset(FsoeError::WatchdogExpired);
      }
      return FsoeCycleResult{
          .state = state_, .txUpdated = true, .rxAccepted = false, .fault = result.fault};
    }
  }

  if (rx.empty()) {
    return result;
  }
  if (lastRxValid_ && std::ranges::equal(rx, lastRx_)) {
    // The same octets as last cycle. A fieldbus repeats the input image until the peer writes new
    // data, and ETG.5100 defines the frame-received event as a Safety PDU in which a bit changed.
    return result;
  }
  std::ranges::copy(rx, lastRx_.begin());
  lastRxValid_ = true;

  return onFrame(rx);
}

// ---- state handlers (ETG.5100 ch. 8.4.2 to 8.4.6)
// ------------------------------------------------

FsoeCycleResult FsoeMaster::onFrame(std::span<const uint8_t> rx) {
  fsoeFrameReadSafeData(rx, rxLayout_, rxSafeData_);
  switch (state_) {
    case FsoeState::Reset:
      return handleReset(rx);
    case FsoeState::Session:
      return handleSession(rx);
    case FsoeState::Connection:
      return handleConnection(rx);
    case FsoeState::Parameter:
      return handleParameter(rx);
    case FsoeState::Data:
      return handleData(rx);
  }
  return FsoeCycleResult{.state = state_};
}

FsoeCycleResult FsoeMaster::handleReset(std::span<const uint8_t> rx) {
  const auto command = static_cast<FsoeCommand>(fsoeFrameCommand(rx));

  if (command == FsoeCommand::Reset) {
    // RESET_OK. The state table checks no CRC here, and it cannot: after a reset both sides are
    // back at their defaults, so there is no chain to check against yet. The session ID exchange
    // that follows is what re-establishes one.
    peerFaultCode_ = rxSafeData_[0];
    startSession(false);
    return FsoeCycleResult{.state = state_, .txUpdated = true, .rxAccepted = true};
  }

  // RESET_STAY1. The slave is somewhere else in the protocol, most often still in Data from a
  // connection this master knows nothing about. Repeat the Reset until it follows.
  const FsoeError fault = emitReset(FsoeError::None);
  return FsoeCycleResult{.state = state_, .txUpdated = true, .rxAccepted = false, .fault = fault};
}

FsoeCycleResult FsoeMaster::handleSession(std::span<const uint8_t> rx) {
  const auto command = static_cast<FsoeCommand>(fsoeFrameCommand(rx));

  if (command == FsoeCommand::Session) {
    if (!checkRecv(rx, lastCrc_, true)) {
      if (secondSessionFrameSent_) {
        return FsoeCycleResult{.state = state_,
                               .txUpdated = true,
                               .rxAccepted = false,
                               .fault = emitReset(FsoeError::InvalidCrc)};
      }
      // SESSION_STAY2. Before the second session frame the table tolerates a bad CRC and sends
      // nothing at all. The watchdog is the only way out, and it ends in Reset.
      startWatchdog();
      return FsoeCycleResult{.state = state_, .txUpdated = false, .rxAccepted = false};
    }

    lastCrc_ = fsoeFrameReadCrc(rx, rxLayout_, 0);
    if (bytesToBeSent_ == 0) {
      // SESSION_OK. The session IDs are exchanged. Send the connection ID and the slave address.
      sendChunk(FsoeCommand::Connection, connData_, kConnDataOctets, 0, lastCrc_,
                config_.connectionId);
      state_ = FsoeState::Connection;
    } else {
      // SESSION_STAY1. Only a 1-octet connection needs a second frame for the session ID.
      sendChunk(FsoeCommand::Session, sessionIdBytes_, kSessionIdOctets,
                static_cast<uint16_t>(kSessionIdOctets - bytesToBeSent_), lastCrc_, 0);
      secondSessionFrameSent_ = true;
    }
    startWatchdog();
    return FsoeCycleResult{.state = state_, .txUpdated = true, .rxAccepted = true};
  }

  if (command == FsoeCommand::Reset) {
    // SESSION_RESET1. The slave asked for a restart. Open a new session at once.
    peerFaultCode_ = rxSafeData_[0];
    startSession(true);
    return FsoeCycleResult{.state = state_, .txUpdated = true, .rxAccepted = true};
  }

  const FsoeError fault = emitReset(fsoeIsKnownCommand(command) ? FsoeError::UnexpectedCommand
                                                                : FsoeError::UnknownCommand);
  return FsoeCycleResult{.state = state_, .txUpdated = true, .rxAccepted = false, .fault = fault};
}

FsoeCycleResult FsoeMaster::handleConnection(std::span<const uint8_t> rx) {
  const auto command = static_cast<FsoeCommand>(fsoeFrameCommand(rx));

  if (command == FsoeCommand::Connection) {
    // The order of these three checks is the order of the state table, and it decides which error
    // code the slave is told. A wrong connection ID is reported as such even when the CRC is also
    // wrong, because the connection ID is the more useful diagnostic.
    if (fsoeFrameConnId(rx, rxLayout_) != config_.connectionId) {
      return FsoeCycleResult{.state = state_,
                             .txUpdated = true,
                             .rxAccepted = false,
                             .fault = emitReset(FsoeError::InvalidConnId)};
    }
    if (!isSafeDataCorrect()) {
      return FsoeCycleResult{.state = state_,
                             .txUpdated = true,
                             .rxAccepted = false,
                             .fault = emitReset(FsoeError::InvalidData)};
    }
    if (!checkRecv(rx, lastCrc_, true)) {
      return FsoeCycleResult{.state = state_,
                             .txUpdated = true,
                             .rxAccepted = false,
                             .fault = emitReset(FsoeError::InvalidCrc)};
    }

    lastCrc_ = fsoeFrameReadCrc(rx, rxLayout_, 0);
    if (bytesToBeSent_ == 0) {
      // CONN_OK. Start the parameter transfer.
      sendChunk(FsoeCommand::Parameter, safePara_, static_cast<uint16_t>(safePara_.size()), 0,
                lastCrc_, config_.connectionId);
      state_ = FsoeState::Parameter;
    } else {
      // CONN_STAY1. The connection data needs more than one frame on a narrow connection.
      sendChunk(FsoeCommand::Connection, connData_, kConnDataOctets,
                static_cast<uint16_t>(kConnDataOctets - bytesToBeSent_), lastCrc_,
                config_.connectionId);
    }
    startWatchdog();
    return FsoeCycleResult{.state = state_, .txUpdated = true, .rxAccepted = true};
  }

  if (command == FsoeCommand::Reset) {
    peerFaultCode_ = rxSafeData_[0];
    startSession(true);
    return FsoeCycleResult{.state = state_, .txUpdated = true, .rxAccepted = true};
  }

  const FsoeError fault = emitReset(fsoeIsKnownCommand(command) ? FsoeError::UnexpectedCommand
                                                                : FsoeError::UnknownCommand);
  return FsoeCycleResult{.state = state_, .txUpdated = true, .rxAccepted = false, .fault = fault};
}

FsoeCycleResult FsoeMaster::handleParameter(std::span<const uint8_t> rx) {
  const auto command = static_cast<FsoeCommand>(fsoeFrameCommand(rx));

  if (command == FsoeCommand::Parameter) {
    if (fsoeFrameConnId(rx, rxLayout_) != config_.connectionId) {
      return FsoeCycleResult{.state = state_,
                             .txUpdated = true,
                             .rxAccepted = false,
                             .fault = emitReset(FsoeError::InvalidConnId)};
    }
    if (!isSafeDataCorrect()) {
      return FsoeCycleResult{.state = state_,
                             .txUpdated = true,
                             .rxAccepted = false,
                             .fault = emitReset(FsoeError::InvalidData)};
    }
    if (!checkRecv(rx, lastCrc_, true)) {
      return FsoeCycleResult{.state = state_,
                             .txUpdated = true,
                             .rxAccepted = false,
                             .fault = emitReset(FsoeError::InvalidCrc)};
    }

    lastCrc_ = fsoeFrameReadCrc(rx, rxLayout_, 0);
    if (bytesToBeSent_ == 0) {
      // PARA_OK. The slave has the whole SafePara and has echoed it. The connection is up, and
      // this frame is the first one that carries real outputs.
      lastCrc_ = sendFrame(dataCommand_, safeOutputs_, lastCrc_, config_.connectionId, true);
      state_ = FsoeState::Data;
    } else {
      // PARA_STAY1. The SafePara is longer than one frame, which is the normal case.
      const auto total = static_cast<uint16_t>(safePara_.size());
      sendChunk(FsoeCommand::Parameter, safePara_, total,
                static_cast<uint16_t>(total - bytesToBeSent_), lastCrc_, config_.connectionId);
    }
    startWatchdog();
    return FsoeCycleResult{.state = state_, .txUpdated = true, .rxAccepted = true};
  }

  if (command == FsoeCommand::Reset) {
    peerFaultCode_ = rxSafeData_[0];
    startSession(true);
    return FsoeCycleResult{.state = state_, .txUpdated = true, .rxAccepted = true};
  }

  const FsoeError fault = emitReset(fsoeIsKnownCommand(command) ? FsoeError::UnexpectedCommand
                                                                : FsoeError::UnknownCommand);
  return FsoeCycleResult{.state = state_, .txUpdated = true, .rxAccepted = false, .fault = fault};
}

FsoeCycleResult FsoeMaster::handleData(std::span<const uint8_t> rx) {
  const auto command = static_cast<FsoeCommand>(fsoeFrameCommand(rx));

  if (command == FsoeCommand::ProcessData || command == FsoeCommand::FailSafeData) {
    if (fsoeFrameConnId(rx, rxLayout_) != config_.connectionId) {
      return FsoeCycleResult{.state = state_,
                             .txUpdated = true,
                             .rxAccepted = false,
                             .fault = emitReset(FsoeError::InvalidConnId)};
    }
    if (!checkRecv(rx, lastCrc_, true)) {
      return FsoeCycleResult{.state = state_,
                             .txUpdated = true,
                             .rxAccepted = false,
                             .fault = emitReset(FsoeError::InvalidCrc)};
    }

    if (command == FsoeCommand::ProcessData) {
      std::ranges::copy(rxSafeData_, safeInputs_.begin());
      inputsValid_ = true;
    } else {
      // DATA_OK2. FailSafeData does carry octets, and they are not process values. Clear the
      // buffer rather than keep the last good one, so a caller that ignores inputsValid still
      // reads the fail-safe value.
      goSafe();
    }

    lastCrc_ = fsoeFrameReadCrc(rx, rxLayout_, 0);
    lastCrc_ = sendFrame(dataCommand_, safeOutputs_, lastCrc_, config_.connectionId, true);
    startWatchdog();
    return FsoeCycleResult{.state = state_, .txUpdated = true, .rxAccepted = true};
  }

  if (command == FsoeCommand::Reset) {
    // DATA_RESET1. The slave dropped the connection and told us why. That code is the one worth
    // showing a user, because the slave saw the fault first.
    peerFaultCode_ = rxSafeData_[0];
    startSession(true);
    return FsoeCycleResult{.state = state_, .txUpdated = true, .rxAccepted = true};
  }

  const FsoeError fault = emitReset(fsoeIsKnownCommand(command) ? FsoeError::UnexpectedCommand
                                                                : FsoeError::UnknownCommand);
  return FsoeCycleResult{.state = state_, .txUpdated = true, .rxAccepted = false, .fault = fault};
}

}  // namespace mm::etg
