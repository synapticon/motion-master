#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "etg/fsoe_crc.h"
#include "etg/fsoe_frame.h"

namespace mm::etg {
namespace fake_slave_detail {

/// Advances a sequence number the way the protocol does: wrap to 1, never to 0.
constexpr uint16_t nextSeq(uint16_t seq) {
  return seq == UINT16_MAX ? 1 : static_cast<uint16_t>(seq + 1);
}

}  // namespace fake_slave_detail

/// @brief A test double for an FSoE slave, good enough to answer a master.
///
/// It is a port of the slave state machine in ETG.5100 ch. 8.5, with the errata sheet applied, and
/// it mirrors the Synapticon firmware slave it was written against. It exists because a master
/// state machine cannot be tested without a peer that authenticates every frame: an echo that
/// accepted anything would let a broken CRC chain or a stalled sequence number pass unnoticed.
///
/// It carries the same envelope as the firmware: SafeData lengths of at least 4 octets, so the
/// session ID and the connection data each fit one frame. Multi-cycle SafePara is supported,
/// because a SafePara of 6 octets or more is normal.
class FakeFsoeSlave {
 public:
  FakeFsoeSlave(uint16_t safeInputsLen, uint16_t safeOutputsLen, uint16_t slaveAddress,
                uint16_t expectedSafeParaSize)
      : inLayout_(safeInputsLen),
        outLayout_(safeOutputsLen),
        chunkLen_(std::min(safeInputsLen, safeOutputsLen)),
        slaveAddress_(slaveAddress),
        expectedSafeParaSize_(expectedSafeParaSize),
        tx_(inLayout_.size(), 0),
        safeInputs_(safeInputsLen, 0),
        safeOutputs_(safeOutputsLen, 0),
        rxSafeData_(safeOutputsLen, 0),
        safePara_(expectedSafeParaSize, 0) {}

  /// @brief Processes one master PDU and returns the answer.
  std::span<const uint8_t> process(std::span<const uint8_t> rx) {
    fsoeFrameReadSafeData(rx, outLayout_, rxSafeData_);
    switch (state_) {
      case FsoeState::Reset:
        handleReset(rx);
        break;
      case FsoeState::Session:
        handleSession(rx);
        break;
      case FsoeState::Connection:
        handleConnection(rx);
        break;
      case FsoeState::Parameter:
        handleParameter(rx);
        break;
      case FsoeState::Data:
        handleData(rx);
        break;
    }
    return tx_;
  }

  void setSafeInputs(std::span<const uint8_t> data) {
    std::ranges::copy(data.first(std::min(data.size(), safeInputs_.size())), safeInputs_.begin());
  }
  /// The command this slave sends in the Data state. It is sticky, unlike the protocol variable:
  /// the firmware's application layer sets it on every cycle, so a test peer that forgets it after
  /// a reset would model the application, not the slave.
  void setDataCommand(FsoeCommand command) { dataCommand_ = command; }

  /// @brief Makes the next answer carry a corrupted CRC, once.
  void corruptNextCrc() { corruptNextCrc_ = true; }

  [[nodiscard]] FsoeState state() const { return state_; }
  [[nodiscard]] std::span<const uint8_t> safeOutputs() const { return safeOutputs_; }
  [[nodiscard]] bool outputsValid() const { return outputsValid_; }
  [[nodiscard]] std::span<const uint8_t> safePara() const { return safePara_; }
  [[nodiscard]] uint16_t connectionId() const { return connectionId_; }

 private:
  uint16_t sendFrame(FsoeCommand command, std::span<const uint8_t> data, uint16_t inheritCrc,
                     uint16_t connId, bool calculateNewCrc) {
    fsoeFrameSetCommand(tx_, static_cast<uint8_t>(command));
    fsoeFrameSetConnId(tx_, inLayout_, connId);
    fsoeFrameWriteSafeData(tx_, inLayout_, data);

    std::array<uint8_t, 2> block0{};
    const uint8_t len0 = fsoeFrameReadBlock(tx_, inLayout_, 0, block0);
    const auto commandOctet = static_cast<uint8_t>(command);

    uint16_t seq = slaveSeq_;
    uint16_t used = seq;
    uint16_t crc0 = 0;
    do {
      used = seq;
      crc0 = fsoeCrc(inheritCrc, connId, used, commandOctet, 0, block0, len0);
      seq = fake_slave_detail::nextSeq(seq);
    } while (calculateNewCrc && crc0 == oldSlaveCrc_);
    fsoeFrameWriteCrc(tx_, inLayout_, 0,
                      corruptNextCrc_ ? static_cast<uint16_t>(crc0 ^ 0x0001) : crc0);
    corruptNextCrc_ = false;

    for (uint16_t k = 1; k < inLayout_.blockCount(); ++k) {
      std::array<uint8_t, 2> block{};
      const uint8_t len = fsoeFrameReadBlock(tx_, inLayout_, k, block);
      fsoeFrameWriteCrc(tx_, inLayout_, k,
                        fsoeCrc(inheritCrc, connId, used, commandOctet, k, block, len));
    }

    slaveSeq_ = seq;
    oldSlaveCrc_ = crc0;
    return crc0;
  }

  bool checkRecv(std::span<const uint8_t> rx, uint16_t inheritCrc, uint16_t seqIn,
                 bool calculateNewCrc) {
    const uint8_t command = fsoeFrameCommand(rx);
    const uint16_t connId = fsoeFrameConnId(rx, outLayout_);

    std::array<uint8_t, 2> block0{};
    const uint8_t len0 = fsoeFrameReadBlock(rx, outLayout_, 0, block0);

    uint16_t seq = seqIn;
    uint16_t used = seq;
    uint16_t crc0 = 0;
    do {
      used = seq;
      crc0 = fsoeCrc(inheritCrc, connId, used, command, 0, block0, len0);
      seq = fake_slave_detail::nextSeq(seq);
    } while (calculateNewCrc && crc0 == oldMasterCrc_);

    bool ok = crc0 == fsoeFrameReadCrc(rx, outLayout_, 0);
    for (uint16_t k = 1; k < outLayout_.blockCount(); ++k) {
      std::array<uint8_t, 2> block{};
      const uint8_t len = fsoeFrameReadBlock(rx, outLayout_, k, block);
      if (fsoeCrc(inheritCrc, connId, used, command, k, block, len) !=
          fsoeFrameReadCrc(rx, outLayout_, k)) {
        ok = false;
      }
    }
    if (ok) {
      masterSeq_ = seq;
      oldMasterCrc_ = crc0;
    }
    return ok;
  }

  void resetVars() {
    lastCrc_ = 0;
    oldMasterCrc_ = 0;
    oldSlaveCrc_ = 0;
    masterSeq_ = 1;
    slaveSeq_ = 1;
    initSeq_ = 1;
  }

  void goSafe() {
    std::ranges::fill(safeOutputs_, 0);
    outputsValid_ = false;
  }

  void emitReset(FsoeError reason) {
    resetVars();
    goSafe();
    std::array<uint8_t, 2> data{static_cast<uint8_t>(reason), 0};
    (void)sendFrame(FsoeCommand::Reset, data, 0, 0, false);
    slaveSeq_ = 1;
    lastCrc_ = 0;
    state_ = FsoeState::Reset;
  }

  /// The echo the master compares against: the first chunk of what arrived, zero-filled.
  std::vector<uint8_t> echo() const {
    std::vector<uint8_t> out(safeInputs_.size(), 0);
    std::ranges::copy(std::span(rxSafeData_).first(chunkLen_), out.begin());
    return out;
  }

  void startSession(uint16_t frameCrc0) {
    sessionId_++;
    std::array<uint8_t, 2> data{static_cast<uint8_t>(sessionId_ & 0xFF),
                                static_cast<uint8_t>(sessionId_ >> 8)};
    lastCrc_ = frameCrc0;
    // The errata sets CalculateNewCrc TRUE here, unlike in the back-to-Reset transitions.
    lastCrc_ = sendFrame(FsoeCommand::Session, data, frameCrc0, 0, true);
    bytesToBeSent_ = remainingAfterChunk(2);
    state_ = FsoeState::Session;
  }

  uint16_t remainingAfterChunk(uint16_t remaining) const {
    return remaining > chunkLen_ ? static_cast<uint16_t>(remaining - chunkLen_) : 0;
  }

  void handleReset(std::span<const uint8_t> rx) {
    const auto command = static_cast<FsoeCommand>(fsoeFrameCommand(rx));
    if (command == FsoeCommand::Session) {
      if (!checkRecv(rx, lastCrc_, masterSeq_, false)) {
        emitReset(FsoeError::InvalidCrc);
        return;
      }
      startSession(fsoeFrameReadCrc(rx, outLayout_, 0));
      return;
    }
    if (command == FsoeCommand::Reset) {
      emitReset(FsoeError::None);
      return;
    }
    emitReset(fsoeIsKnownCommand(command) ? FsoeError::UnexpectedCommand
                                          : FsoeError::UnknownCommand);
  }

  void handleSession(std::span<const uint8_t> rx) {
    const auto command = static_cast<FsoeCommand>(fsoeFrameCommand(rx));
    const uint16_t connId = fsoeFrameConnId(rx, outLayout_);

    if (command == FsoeCommand::Connection) {
      if (connId == 0) {
        emitReset(FsoeError::InvalidConnId);
        return;
      }
      if (!checkRecv(rx, lastCrc_, masterSeq_, true)) {
        emitReset(FsoeError::InvalidCrc);
        return;
      }
      const uint16_t frameCrc0 = fsoeFrameReadCrc(rx, outLayout_, 0);
      connectionData_ = {rxSafeData_[0], rxSafeData_[1], rxSafeData_[2], rxSafeData_[3]};
      connectionId_ = connId;
      lastCrc_ = sendFrame(FsoeCommand::Connection, echo(), frameCrc0, connectionId_, true);
      bytesToBeSent_ = remainingAfterChunk(4);
      state_ = FsoeState::Connection;
      return;
    }
    if (command == FsoeCommand::Session) {
      if (!checkRecv(rx, 0, initSeq_, false)) {
        emitReset(FsoeError::InvalidCrc);
        return;
      }
      resetVars();
      startSession(fsoeFrameReadCrc(rx, outLayout_, 0));
      return;
    }
    if (command == FsoeCommand::Reset) {
      emitReset(FsoeError::None);
      return;
    }
    emitReset(fsoeIsKnownCommand(command) ? FsoeError::UnexpectedCommand
                                          : FsoeError::UnknownCommand);
  }

  void handleConnection(std::span<const uint8_t> rx) {
    const auto command = static_cast<FsoeCommand>(fsoeFrameCommand(rx));
    const uint16_t connId = fsoeFrameConnId(rx, outLayout_);
    const auto storedConnId = static_cast<uint16_t>(connectionData_[0] | (connectionData_[1] << 8));
    const auto storedAddress =
        static_cast<uint16_t>(connectionData_[2] | (connectionData_[3] << 8));

    if (command == FsoeCommand::Parameter) {
      if (connId != connectionId_ || storedConnId != connectionId_) {
        emitReset(FsoeError::InvalidConnId);
        return;
      }
      if (storedAddress != slaveAddress_) {
        emitReset(FsoeError::InvalidAddress);
        return;
      }
      if (!checkRecv(rx, lastCrc_, masterSeq_, true)) {
        emitReset(FsoeError::InvalidCrc);
        return;
      }
      const uint16_t frameCrc0 = fsoeFrameReadCrc(rx, outLayout_, 0);
      storeSafePara(0);
      lastCrc_ = sendFrame(FsoeCommand::Parameter, echo(), frameCrc0, connectionId_, true);
      bytesToBeSent_ = remainingAfterChunk(expectedSafeParaSize_);
      state_ = FsoeState::Parameter;
      return;
    }
    if (command == FsoeCommand::Reset) {
      emitReset(FsoeError::None);
      return;
    }
    emitReset(fsoeIsKnownCommand(command) ? FsoeError::UnexpectedCommand
                                          : FsoeError::UnknownCommand);
  }

  void handleParameter(std::span<const uint8_t> rx) {
    const auto command = static_cast<FsoeCommand>(fsoeFrameCommand(rx));
    const uint16_t connId = fsoeFrameConnId(rx, outLayout_);

    if (command == FsoeCommand::ProcessData || command == FsoeCommand::FailSafeData) {
      if (bytesToBeSent_ != 0) {
        emitReset(FsoeError::UnexpectedCommand);
        return;
      }
      if (connId != connectionId_) {
        emitReset(FsoeError::InvalidConnId);
        return;
      }
      if (const FsoeError fault = validateSafePara(); fault != FsoeError::None) {
        emitReset(fault);
        return;
      }
      if (!checkRecv(rx, lastCrc_, masterSeq_, true)) {
        emitReset(FsoeError::InvalidCrc);
        return;
      }
      applySafeOutputs(command);
      state_ = FsoeState::Data;
      sendDataResponse(fsoeFrameReadCrc(rx, outLayout_, 0));
      return;
    }
    if (command == FsoeCommand::Parameter) {
      if (bytesToBeSent_ == 0) {
        emitReset(FsoeError::UnexpectedCommand);
        return;
      }
      if (!checkRecv(rx, lastCrc_, masterSeq_, true)) {
        emitReset(FsoeError::InvalidCrc);
        return;
      }
      const uint16_t frameCrc0 = fsoeFrameReadCrc(rx, outLayout_, 0);
      storeSafePara(static_cast<uint16_t>(expectedSafeParaSize_ - bytesToBeSent_));
      lastCrc_ = sendFrame(FsoeCommand::Parameter, echo(), frameCrc0, connectionId_, true);
      bytesToBeSent_ = remainingAfterChunk(bytesToBeSent_);
      return;
    }
    if (command == FsoeCommand::Reset) {
      emitReset(FsoeError::None);
      return;
    }
    emitReset(fsoeIsKnownCommand(command) ? FsoeError::UnexpectedCommand
                                          : FsoeError::UnknownCommand);
  }

  void handleData(std::span<const uint8_t> rx) {
    const auto command = static_cast<FsoeCommand>(fsoeFrameCommand(rx));
    const uint16_t connId = fsoeFrameConnId(rx, outLayout_);

    if (command == FsoeCommand::ProcessData || command == FsoeCommand::FailSafeData) {
      if (connId != connectionId_) {
        emitReset(FsoeError::InvalidConnId);
        return;
      }
      if (!checkRecv(rx, lastCrc_, masterSeq_, true)) {
        emitReset(FsoeError::InvalidCrc);
        return;
      }
      applySafeOutputs(command);
      sendDataResponse(fsoeFrameReadCrc(rx, outLayout_, 0));
      return;
    }
    if (command == FsoeCommand::Reset) {
      emitReset(FsoeError::None);
      return;
    }
    emitReset(fsoeIsKnownCommand(command) ? FsoeError::UnexpectedCommand
                                          : FsoeError::UnknownCommand);
  }

  void sendDataResponse(uint16_t frameCrc0) {
    lastCrc_ = sendFrame(dataCommand_, safeInputs_, frameCrc0, connectionId_, true);
  }

  void applySafeOutputs(FsoeCommand command) {
    if (command == FsoeCommand::ProcessData) {
      std::ranges::copy(rxSafeData_, safeOutputs_.begin());
      outputsValid_ = true;
    } else {
      goSafe();
    }
  }

  void storeSafePara(uint16_t offset) {
    for (uint16_t i = 0; i < chunkLen_ && (offset + i) < expectedSafeParaSize_; ++i) {
      safePara_[offset + i] = rxSafeData_[i];
    }
  }

  FsoeError validateSafePara() const {
    const auto commParaLen = static_cast<uint16_t>(safePara_[0] | (safePara_[1] << 8));
    const auto watchdogMs = static_cast<uint16_t>(safePara_[2] | (safePara_[3] << 8));
    const auto appParaLen = static_cast<uint16_t>(safePara_[4] | (safePara_[5] << 8));
    if (commParaLen != 2) {
      return FsoeError::InvalidCommParaLen;
    }
    if (watchdogMs == 0) {
      return FsoeError::InvalidCommPara;
    }
    if (appParaLen != expectedSafeParaSize_ - 6) {
      return FsoeError::InvalidAppParaLen;
    }
    return FsoeError::None;
  }

  FsoeFrameLayout inLayout_;   ///< Layout of the PDUs this slave sends.
  FsoeFrameLayout outLayout_;  ///< Layout of the PDUs the master sends.
  uint16_t chunkLen_;
  uint16_t slaveAddress_;
  uint16_t expectedSafeParaSize_;

  std::vector<uint8_t> tx_;
  std::vector<uint8_t> safeInputs_;
  std::vector<uint8_t> safeOutputs_;
  std::vector<uint8_t> rxSafeData_;
  std::vector<uint8_t> safePara_;
  std::array<uint8_t, 4> connectionData_{};

  FsoeState state_ = FsoeState::Reset;
  FsoeCommand dataCommand_ = FsoeCommand::ProcessData;
  uint16_t lastCrc_ = 0;
  uint16_t oldMasterCrc_ = 0;
  uint16_t oldSlaveCrc_ = 0;
  uint16_t masterSeq_ = 1;
  uint16_t slaveSeq_ = 1;
  uint16_t initSeq_ = 1;
  uint16_t sessionId_ = 0x1000;
  uint16_t bytesToBeSent_ = 0;
  uint16_t connectionId_ = 0;
  bool outputsValid_ = false;
  bool corruptNextCrc_ = false;
};

}  // namespace mm::etg
