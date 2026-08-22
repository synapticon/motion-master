#pragma once

#include <cstdint>
#include <span>

namespace mm::etg {

/// @brief Computes one FSoE @c CRC_i over one Safety PDU block (ETG.5100 ch. 8.1.3, Annex A).
///
/// A Safety PDU carries no single checksum over the whole frame. The protocol cuts SafeData into
/// blocks of at most two octets, and **each block carries its own CRC**. That is what makes the
/// frame grow as @c 1+2n+2 rather than @c 1+n+2, and it is why this function computes the CRC of
/// one block rather than of a frame. @c fsoePduBuildCrcs (etg/fsoe_frame.h) drives it over a whole
/// PDU.
///
/// The octet sequence fed to the register comes from ETG.5100 Tables 6 and 7, and from the Annex A
/// reference function @c CalcCrc:
///
///     recvCrc0(lo,hi), connId(lo,hi), seqNo(lo,hi), command,
///     [index(lo,hi) - only when index > 0], block[0], [block[1] - only when blockLen == 2]
///
/// Two details are easy to get wrong, and both are settled here the way the reference does it. The
/// register starts at **0**. **No trailing zero octets are appended.** Parts of the prose read as
/// if one or both were otherwise, but conformance is measured against the Annex A reference, and
/// that code does neither.
///
/// @p recvCrc0 is @c CRC_0 of the last PDU received from the peer. It chains the two directions
/// together, so a stale or replayed frame fails even when its own contents are self-consistent.
/// It is 0 until the first PDU arrives.
///
/// @param recvCrc0 @c CRC_0 of the last received PDU; 0 on reset.
/// @param connId   connection ID carried in the PDU; 0 in the Reset and Session states.
/// @param seqNo    the sender's current 16-bit sequence number.
/// @param command  the FSoE command octet.
/// @param index    block index @c i. Index 0 selects @c CRC_0, for which no index octets are fed
///                 in.
/// @param block    the block's SafeData octets. Only the first @p blockLen are read.
/// @param blockLen SafeData octets in this block, 1 or 2. Any other value yields 0.
/// @return the 16-bit @c CRC_i.
uint16_t fsoeCrc(uint16_t recvCrc0, uint16_t connId, uint16_t seqNo, uint8_t command,
                 uint16_t index, std::span<const uint8_t> block, uint8_t blockLen);

/// @brief Returns the first normative ETG.5100 Annex A table. Exposed for verification only.
///
/// The tests re-derive table 1 from the Safety polynomial and check both tables for the linearity
/// every CRC table has. Nothing else needs them.
std::span<const uint16_t> fsoeCrcTable1();

/// @copydoc fsoeCrcTable1
std::span<const uint16_t> fsoeCrcTable2();

}  // namespace mm::etg
