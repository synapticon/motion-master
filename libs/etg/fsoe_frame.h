#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace mm::etg {

/// @brief Octets in the smallest Safety PDU: command, one SafeData octet, @c CRC_0, connection ID.
inline constexpr uint16_t kFsoeMinPduSize = 6;

/// @brief FSoE commands (ETG.5100 ch. 8.1.2).
///
/// The values are not an ordering. Each one is a fixed 8-bit pattern with a large Hamming distance
/// to the others, so a corrupted command octet does not decode as a different valid command.
enum class FsoeCommand : uint8_t {
  Reset = 0x2A,
  Session = 0x4E,
  Connection = 0x64,
  Parameter = 0x52,
  ProcessData = 0x36,
  FailSafeData = 0x08,
};

/// @brief Communication error codes, carried in @c SafeData[0] of a Reset PDU (ETG.5100 Table 28).
///
/// Codes 0x80 to 0xFF are device-specific SafePara faults and are deliberately not enumerated.
enum class FsoeError : uint8_t {
  None = 0,  ///< A local reset, or the acknowledgement of one. Not a fault.
  UnexpectedCommand = 1,
  UnknownCommand = 2,
  InvalidConnId = 3,
  InvalidCrc = 4,
  WatchdogExpired = 5,
  InvalidAddress = 6,
  InvalidData = 7,
  InvalidCommParaLen = 8,
  InvalidCommPara = 9,
  InvalidAppParaLen = 10,
  InvalidAppPara = 11,
};

/// @brief Returns a short name for an FSoE command. An unknown value renders as @c "?".
constexpr std::string_view fsoeCommandName(FsoeCommand v) {
  switch (v) {
    case FsoeCommand::Reset:
      return "Reset";
    case FsoeCommand::Session:
      return "Session";
    case FsoeCommand::Connection:
      return "Connection";
    case FsoeCommand::Parameter:
      return "Parameter";
    case FsoeCommand::ProcessData:
      return "ProcessData";
    case FsoeCommand::FailSafeData:
      return "FailSafeData";
  }
  return "?";
}

/// @brief Returns whether @p v is one of the six commands the standard defines.
///
/// A receiver answers an unknown command with UNKNOWN_CMD and a known but out-of-order one with
/// UNEXPECTED_CMD, so the two cases have to stay apart.
constexpr bool fsoeIsKnownCommand(FsoeCommand v) {
  switch (v) {
    case FsoeCommand::Reset:
    case FsoeCommand::Session:
    case FsoeCommand::Connection:
    case FsoeCommand::Parameter:
    case FsoeCommand::ProcessData:
    case FsoeCommand::FailSafeData:
      return true;
  }
  return false;
}

/// @brief Returns a short name for an FSoE error code. An unknown value renders as @c "?".
constexpr std::string_view fsoeErrorName(FsoeError v) {
  switch (v) {
    case FsoeError::None:
      return "None";
    case FsoeError::UnexpectedCommand:
      return "UnexpectedCommand";
    case FsoeError::UnknownCommand:
      return "UnknownCommand";
    case FsoeError::InvalidConnId:
      return "InvalidConnId";
    case FsoeError::InvalidCrc:
      return "InvalidCrc";
    case FsoeError::WatchdogExpired:
      return "WatchdogExpired";
    case FsoeError::InvalidAddress:
      return "InvalidAddress";
    case FsoeError::InvalidData:
      return "InvalidData";
    case FsoeError::InvalidCommParaLen:
      return "InvalidCommParaLen";
    case FsoeError::InvalidCommPara:
      return "InvalidCommPara";
    case FsoeError::InvalidAppParaLen:
      return "InvalidAppParaLen";
    case FsoeError::InvalidAppPara:
      return "InvalidAppPara";
  }
  return "?";
}

/// @brief States of an FSoE connection (ETG.5100 Tables 29 and 34).
///
/// The master and the slave run the same five states, and a connection is up only when both are
/// in Data.
enum class FsoeState : uint8_t {
  Reset,       ///< The connection is down. SafeInputs read fail-safe.
  Session,     ///< The two session IDs are in transfer.
  Connection,  ///< The connection ID and the slave address are in transfer.
  Parameter,   ///< The SafePara is in transfer.
  Data,        ///< Process data or fail-safe data is in transfer.
};

/// @brief Returns a short name for a connection state. An unknown value renders as @c "?".
constexpr std::string_view fsoeStateName(FsoeState v) {
  switch (v) {
    case FsoeState::Reset:
      return "Reset";
    case FsoeState::Session:
      return "Session";
    case FsoeState::Connection:
      return "Connection";
    case FsoeState::Parameter:
      return "Parameter";
    case FsoeState::Data:
      return "Data";
  }
  return "?";
}

/// @brief Locates every field of a Safety PDU that carries @c n SafeData octets (ETG.5100 ch. 8.1).
///
/// The layout is not "header, payload, checksum". SafeData is cut into blocks of at most two
/// octets, and **each block is followed by its own 2-octet CRC**. The connection ID comes last:
///
///     n = 2:   cmd  d0 d1  crc0(2)                            connId(2)  ->   7 octets
///     n = 4:   cmd  d0 d1  crc0(2)  d2 d3  crc1(2)            connId(2)  ->  11 octets
///     n = 12:  cmd  <six data and CRC blocks>                 connId(2)  ->  27 octets
///
/// So a frame is @c 1+2n+2 octets, not @c 1+n+2. Size a PDO from the wrong formula and the
/// exchange fails silently: the peer sees a short frame and drops it without an error.
///
/// The standard makes @c n==1 a special case with one 1-octet block, which gives the 6-octet
/// minimum frame (@c kFsoeMinPduSize).
///
/// Only @c n==1 and an even @c n are legal. Any other value yields an invalid layout whose
/// @c size() and @c blockCount() are 0, rather than plausible offsets into a frame that cannot
/// exist.
class FsoeFrameLayout {
 public:
  explicit constexpr FsoeFrameLayout(uint16_t safeDataLen) : safeDataLen_(safeDataLen) {}

  /// @brief Returns whether @c n is a length the standard permits: 1, or a positive even number.
  [[nodiscard]] constexpr bool valid() const {
    return safeDataLen_ == 1 || (safeDataLen_ != 0 && safeDataLen_ % 2 == 0);
  }

  [[nodiscard]] constexpr uint16_t safeDataLen() const { return safeDataLen_; }

  /// @brief Returns the total PDU size in octets, or 0 if the length is not legal.
  [[nodiscard]] constexpr uint16_t size() const {
    if (safeDataLen_ == 1) {
      return kFsoeMinPduSize;
    }
    return valid() ? static_cast<uint16_t>(1 + 2 * safeDataLen_ + 2) : 0;
  }

  /// @brief Returns the number of data and CRC blocks, or 0 if the length is not legal.
  [[nodiscard]] constexpr uint16_t blockCount() const {
    if (safeDataLen_ == 1) {
      return 1;
    }
    return valid() ? static_cast<uint16_t>(safeDataLen_ / 2) : 0;
  }

  /// @brief Returns the SafeData octets per block: 1 in the @c n==1 case, otherwise 2.
  [[nodiscard]] constexpr uint16_t blockDataLen() const { return safeDataLen_ == 1 ? 1 : 2; }

  /// @brief Returns the offset of the first SafeData octet of block @p k.
  [[nodiscard]] constexpr uint16_t dataOffset(uint16_t k) const {
    return safeDataLen_ == 1 ? 1 : static_cast<uint16_t>(1 + 4 * k);
  }

  /// @brief Returns the offset of the low octet of the CRC of block @p k.
  [[nodiscard]] constexpr uint16_t crcOffset(uint16_t k) const {
    return static_cast<uint16_t>(dataOffset(k) + blockDataLen());
  }

  /// @brief Returns the offset of the low octet of the connection ID.
  [[nodiscard]] constexpr uint16_t connIdOffset() const {
    return safeDataLen_ == 1 ? 4 : static_cast<uint16_t>(1 + 4 * blockCount());
  }

 private:
  uint16_t safeDataLen_;
};

/// @brief The outcome of a CRC check over a whole received PDU.
struct FsoeCheckResult {
  /// True only when the layout is valid and every block CRC matches.
  bool crcOk = false;
  /// The **computed** @c CRC_0, which is what chains into the next frame. On a mismatch this is
  /// the local value and not the one that arrived, so do not chain it without reading @c crcOk.
  uint16_t crc0 = 0;
};

/// @brief Returns the command octet, or 0 when @p pdu is empty.
uint8_t fsoeFrameCommand(std::span<const uint8_t> pdu);

/// @brief Writes the command octet. An empty @p pdu is ignored.
void fsoeFrameSetCommand(std::span<uint8_t> pdu, uint8_t command);

/// @brief Returns the connection ID, or 0 when @p pdu is too short.
uint16_t fsoeFrameConnId(std::span<const uint8_t> pdu, const FsoeFrameLayout& layout);

/// @brief Writes the connection ID. A @p pdu that is too short is ignored.
void fsoeFrameSetConnId(std::span<uint8_t> pdu, const FsoeFrameLayout& layout, uint16_t connId);

/// @brief Copies the SafeData octets of block @p k into @p out.
/// @return the number of octets written, 1 or 2, or 0 when a span is too short or @p k is out of
///         range.
uint8_t fsoeFrameReadBlock(std::span<const uint8_t> pdu, const FsoeFrameLayout& layout, uint16_t k,
                           std::span<uint8_t> out);

/// @brief Writes up to @p len SafeData octets into block @p k. An out-of-range @p k is ignored.
void fsoeFrameWriteBlock(std::span<uint8_t> pdu, const FsoeFrameLayout& layout, uint16_t k,
                         std::span<const uint8_t> in, uint8_t len);

/// @brief Returns the CRC of block @p k, or 0 when @p k is out of range.
uint16_t fsoeFrameReadCrc(std::span<const uint8_t> pdu, const FsoeFrameLayout& layout, uint16_t k);

/// @brief Writes the CRC of block @p k. An out-of-range @p k is ignored.
void fsoeFrameWriteCrc(std::span<uint8_t> pdu, const FsoeFrameLayout& layout, uint16_t k,
                       uint16_t crc);

/// @brief Copies the SafeData of every block into one linear buffer.
/// @return the number of octets written, which is @c min(n, out.size()).
uint16_t fsoeFrameReadSafeData(std::span<const uint8_t> pdu, const FsoeFrameLayout& layout,
                               std::span<uint8_t> out);

/// @brief Writes one linear SafeData buffer into the blocks of a PDU, zero-filling any shortfall.
void fsoeFrameWriteSafeData(std::span<uint8_t> pdu, const FsoeFrameLayout& layout,
                            std::span<const uint8_t> data);

/// @brief Fills in every block CRC of a PDU whose command, SafeData and connection ID are set.
/// @return @c CRC_0, which the peer needs as @p recvCrc0 when it validates the next frame.
uint16_t fsoePduBuildCrcs(std::span<uint8_t> pdu, const FsoeFrameLayout& layout, uint16_t recvCrc0,
                          uint16_t seqNo);

/// @brief Recomputes every block CRC of a received PDU and compares.
///
/// Every block is checked even after one has failed. An early exit would make the cost of
/// validation depend on where the corruption sits, and the caller wants @c crc0 either way.
FsoeCheckResult fsoePduCheck(std::span<const uint8_t> pdu, const FsoeFrameLayout& layout,
                             uint16_t recvCrc0, uint16_t seqNo);

}  // namespace mm::etg
