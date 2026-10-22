#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/// @file
/// @brief One EtherCAT frame as it sits on the wire, read from its bytes.

namespace mm::capture {

/// @brief The Ethernet type of an EtherCAT frame.
inline constexpr uint16_t kEtherCatEtherType = 0x88A4;

/// @brief The header of one datagram in a frame, without its data.
struct Datagram {
  uint8_t command = 0;  ///< ETG.1000.4 command: 1 APRD, 4 FPRD, 10 LRD, 12 LRW, and so on.
  uint8_t index = 0;    ///< The master's index, which is how it pairs a return with a send.
  uint16_t adp = 0;     ///< Address position, or the high half of a logical address.
  uint16_t ado = 0;     ///< Address offset, or the low half of a logical address.
  uint16_t length = 0;  ///< Bytes of data in the datagram.
  bool more = false;    ///< Another datagram follows in the same frame.
  uint16_t wkc = 0;     ///< The working counter after the data.
};

/// @brief One EtherCAT frame.
struct EtherCatFrame {
  /// True when the master sent this frame and no slave has processed it yet. Every ESC sets the
  /// locally administered bit of the first source-MAC byte on a frame it processes (ESC datasheet,
  /// Section I, 3), so a frame on its way back to the master has that bit set. The rule reads any
  /// capture, from this program or from tcpdump, with no knowledge of the master's MAC.
  bool fromMaster = false;
  std::vector<Datagram> datagrams;  ///< In wire order. Never empty.
};

/// @brief Whether @p command addresses the process image: LRD, LWR or LRW.
///
/// The cyclic frame of the RT loop is the one whose first datagram is logical. Everything else is
/// the control plane: mailbox and register reads and writes.
bool isLogicalCommand(uint8_t command);

/// @brief Whether every slave counts @p command's position up as the frame passes.
///
/// Auto-increment and broadcast commands come back with another position than they went out with,
/// so a match of a return to its send leaves the position out for them.
bool isPositionCountedUp(uint8_t command);

/// @brief Reads one Ethernet frame that carries EtherCAT datagrams.
///
/// @param bytes  The frame from the Ethernet header on, without a frame check sequence.
/// @return The frame, or @c nullopt when it is not an EtherCAT frame of type 1 (EtherCAT command),
///         or its datagrams do not fit its length.
std::optional<EtherCatFrame> parseEtherCatFrame(std::span<const uint8_t> bytes);

}  // namespace mm::capture
